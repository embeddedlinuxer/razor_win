/* This Information is proprietary to Phase Dynamics Inc, Richardson, Texas 
* and MAY NOT be copied by any method or incorporated into another program
* without the express written consent of Phase Dynamics Inc. This information
* or any portion thereof remains the property of Phase Dynamics Inc.
* The information contained herein is believed to be accurate and Phase
* Dynamics Inc assumes no responsibility or liability for its use in any way
* and conveys no license or title under any patent or copyright and makes
* no representation or warranty that this Information is free from patent
* or copyright infringement.
*------------------------------------------------------------------------

*------------------------------------------------------------------------
* Menu.c
*-------------------------------------------------------------------------
* Contains all the code relevant to the LCD menu system. The menu system
* uses a state-machine architecture, which is defined in Menu.h by a state
* table and a state transition table. This is the only Razor code that is
* run in the context of a Task (SYS-BIOS module) – everything else is
* interrupt-based.
* Most "end-node" states of the menu tree have a function associated with
* them. Each MNU_xxxx menu calls the corresponding FXN_xxxx function. 
* Currently, we cycle through the mnu
* code with a maximum frequency of about 6.67 times per second
* (minimum period = 0.15 seconds).
*------------------------------------------------------------------------*/

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ti/sysbios/hal/Seconds.h>
#include "Globals.h"
#include "nandwriter.h"
#include "Errors.h"
#include "Utils.h"
#include "Watchdog.h"
#include "ModbusRTU.h"
#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#define MENU_H
#include "Menu.h"

static int blinker = 0;             		// to control menu id blinker 
static BOOL isOn = FALSE;           		// to control lcd line_1 blinker
static BOOL isMessage = FALSE;      		// to display pass/fail/cancel message
static BOOL isTechModeRequested = FALSE; 	// to access technicial mode

//////////////////////////////////////////////////////////////
/// oil capture progress bar
//////////////////////////////////////////////////////////////

static char prg0[]  = "[..............]";
static char prg1[]  = "[#.............]";
static char prg2[]  = "[##............]";
static char prg3[]  = "[###...........]";
static char prg4[]  = "[####..........]";
static char prg5[]  = "[#####.........]";
static char prg6[]  = "[######........]";
static char prg7[]  = "[#######.......]";
static char prg8[]  = "[########......]";
static char prg9[]  = "[#########.....]";
static char prg10[] = "[##########....]";
static char prg11[] = "[###########...]";
static char prg12[] = "[############..]";
static char prg13[] = "[#############.]";
static char prg14[] = "[##############]";

//////////////////////////////////////////////////////////////
///	density units
//////////////////////////////////////////////////////////////

static const char * statusMode[2]    = {RELAY_OFF, RELAY_ON}; 
static const char * phaseMode[2]     = {WATER_PHASE, OIL_PHASE}; 
static const char * relayMode[4]     = {WATERCUT, PHASE, ERROR, MANUAL}; 
static const char * aoMode[3] 		 = {AUTOMATIC, AUTO_REVERSE, MANUAL}; 
static const char * errorMode[3] 	 = {DISABLE, AO_ALARM_HIGH, AO_ALARM_LOW}; 
static const char * densityMode[4] 	 = {DISABLE, ANALOG_INPUT, MODBUS, MANUAL};
static const char * densityIndex[8]   = {KG_M3, KG_M3_15C, API, API_60F, LBS_FT3, LBS_FT3_60F, SG, SG_15C}; 
static const Uint8 densityUnit[8] 	 = {u_mpv_kg_cm, u_mpv_kg_cm_15C, u_mpv_deg_API, u_mpv_deg_API_60F, u_mpv_lbs_cf, u_mpv_lbs_cf_60F, u_mpv_sg, u_mpv_sg_15C};
static const char * USB_CODE[18] 	 = {DISABLED,ENABLED,USB_ERROR2,USB_ERROR3,USB_ERROR4,USB_ERROR5,USB_ERROR6,USB_ERROR7,USB_ERROR8,USB_ERROR9,USB_ERROR10,USB_ERROR11,USB_ERROR12,USB_ERROR13,USB_ERROR14,USB_ERROR15,USB_ERROR16,USB_ERROR17};

//////////////////////////////////////////////////////////////
/// function definitions
//////////////////////////////////////////////////////////////

void setupMenu (void)
{
	MENU.state			= MNU_HOMESCREEN_WTC; // set initial screen to Water Cut
	MENU.dir 			= MNU_DIR_RIGHT;
	MENU.debounceDone 	= TRUE;
	MENU.isPressAndHold	= FALSE;
	MENU.isHomeScreen 	= FALSE;			  // trigger to homescreen
	MENU.col 			= NULL;
	MENU.row 			= NULL;
	MENU.curStat 		= LCD_CURS_OFF | LCD_CURS_NOBLINK;
}

//////////////////////////////////////////////////////////////
/// ISR_Process_Menu()
/// Clock Handle: Process_Menu_Clock
//////////////////////////////////////////////////////////////

void 
ISR_Process_Menu (void)
{
	Semaphore_post(Menu_sem);
}

//////////////////////////////////////////////////////////////
/// ISR_DataLogging()
/// Clock Handle: logData_Clock
//////////////////////////////////////////////////////////////

void 
ISR_logData(void)
{
	if (isLogData) Semaphore_post(logData_sem);
}

//////////////////////////////////////////////////////////////
/// Process_Menu()
/// Task Handle:		Menu_task 
/// Semaphore Pend:	Menu_sem
//////////////////////////////////////////////////////////////

void 
Process_Menu(void)
{
	char 	prevButtons[4];
	Uint32	buttons[4];
	Uint32	key;
	Uint16 	btnIndex = NULL;
	Uint16 	nextState = MENU.state;
	Uint8	needRelayClick;
	static 	Uint16 (*stateFxn)(Uint16);
    Uint8   isValidInput = TRUE;
	int i;
	
	/* function pointer */
	stateFxn = MENU_TABLE[0].fxnPtr;

	/* Initialize menu */
	setupMenu();						 
    
    /// Initialize buttons
	for (i=0; i<4; i++) buttons[i] = 0; 

	/// Start main loop
	while (1)
	{
		/* Reset watchdog timer. Otherwise, resets the system. */
     	TimerWatchdogReactivate(CSL_TMR_1_REGS);

        if (COIL_UPDATE_FACTORY_DEFAULT.val) storeUserDataToFactoryDefault();
		if (!COIL_LOCKED_SOFT_FACTORY_RESET.val && !COIL_LOCKED_HARD_FACTORY_RESET.val) 
		{
			Swi_post(Swi_writeNand);
			while(1);
		}

		Semaphore_pend(Menu_sem, BIOS_WAIT_FOREVER); 		// wait until next Menu_sem post
		needRelayClick 	= FALSE; 							// this is a great place for a breakpoint!
		nextState 		= MENU.state;						// check next state
		btnIndex 		= BTN_NONE;							// check button selected
		key 			= Swi_disable();
        isValidInput    = TRUE;

		////////////////////////////////////////////////////////////////////////////
		//// BLINK CONTROLLER
		////////////////////////////////////////////////////////////////////////////
        
        (blinker < 5) ? (isOn = TRUE) : (isOn = FALSE);
        (blinker > 8) ? (blinker = 0) : (blinker++);

		////////////////////////////////////////////////////////////////////////////
		/// Note: I2C_BUTTON_STATUS_X is updated regularly with I2C_Pulse_MBVE_Clock
		/// Update buttons[] with button status
		////////////////////////////////////////////////////////////////////////////

		buttons[0] = I2C_BUTTON_STEP;					
		buttons[1] = I2C_BUTTON_VALUE;					 
		buttons[2] = I2C_BUTTON_ENTER;					
		buttons[3] = I2C_BUTTON_BACK;					

        if ((buttons[0] + buttons[1] + buttons[2] + buttons[3]) > 1) isValidInput = FALSE;

		////////////////////////////////////////////////////////////////////////////
		/// HANDLE BUTTONS PRESSED
		////////////////////////////////////////////////////////////////////////////

        if (isValidInput)        
        {
            for (i=0; i<4; i++)
		    {
			    if (buttons[i] != prevButtons[i])				// button pressed?
			    {
				    if (buttons[i] == 1) 						// rising edge?
				    {
					    if (MENU.debounceDone) 					// debounce clock finished?
					    {
						    btnIndex 			= i;			// button index
						    prevButtons[i]		= buttons[i];
						    MENU.debounceDone 	= FALSE;
						    Clock_start(DebounceMBVE_Clock);	// start the debounce clock
						    needRelayClick 		= TRUE;

     						TimerWatchdogReactivate(CSL_TMR_1_REGS);
					    }
				    }
				    else										// falling edge
				    { 
					    prevButtons[i] 		= buttons[i];
					    MENU.isPressAndHold	= FALSE; 			// mnu button no longer being held down
				    }
						
                    break;									
			    }
		    }
        }

		Swi_restore(key);

		////////////////////////////////////////////////////////////////////////////
		// NEXT STATE
		////////////////////////////////////////////////////////////////////////////

		nextState = stateFxn(btnIndex); 				// next state
           
		////////////////////////////////////////////////////////////////////////////
        // UPDATE STATE	
        ////////////////////////////////////////////////////////////////////////////	

		DisableButtonInts(); // ADDED BY DKOH - JUN 13, 2019
		key = Swi_disable();

		if (MENU.state != nextState) 						// is menu changed?
		{
			MENU.state = nextState;							// next to current
			i = 0;

			while (MENU_TABLE[i].state != NULL) 					
			{ 
				if (MENU_TABLE[i].state == MENU.state) 		// found matching menu 
				{
					MENU.id 	= MENU_TABLE[i].id+'0';		// int to txt
					MENU.pos	= MENU_TABLE[i].pos;  	 	// blinking position
					stateFxn 	= MENU_TABLE[i].fxnPtr;		// function pointer
                    blinker     = 0;                        // reset blinker

					break; 
				}

				i++;
			}
		}

        Swi_restore(key);
		EnableButtonInts();

        /// WILL BLINK MENU ID
        blinkMenu();

		/// Reset watchdog timer. Otherwise, resets the system. 
     	TimerWatchdogReactivate(CSL_TMR_1_REGS);
	}
}


void
blinkMenu(void)
{
    if (MENU.id < '0' || MENU.id > '9') return; 
    (isOn) ? LCD_printch(MENU.id, MENU.pos, 0) : LCD_printch(' ', MENU.pos, 0);
	LCD_setaddr(MENU.col,MENU.row); 	// restore cursor position
}


void
blinkLcdLine1(const char * textA, const char * textB)
{
	(isOn) ? displayLcd(textA, LCD1) : displayLcd(textB, LCD1);
	LCD_setaddr(MENU.col,MENU.row); 	// restore cursor position
}

void 
DebounceMBVE(void)
{
	MENU.debounceDone = TRUE;
}


int
notifyMessageAndExit(const int currentId, const int nextId)
{
    LCD_setcursor(0,0);

    if (counter < 8) 
    {
        displayLcd(lcdLine1, LCD1);
        counter++;
    }
    else
    {
        counter = 0;
        isMessage = FALSE;
        isUpdateDisplay = TRUE;
        return nextId;
    }

    return currentId;
}


int
onFxnBackPressed(const int currentId)
{
    isMessage = TRUE;
	memcpy(lcdLine1,CANCEL,16);
    return currentId;
}


int onNextPressed(const int nextId)
{
    LCD_setcursor(0,0);
    counter = 0;
    isUpdateDisplay = TRUE;
    isMessage = FALSE;
    return nextId;
}


int onMnuStepPressed(const int nextId, const int currentId, const char * label)
{
    isUpdateDisplay = TRUE;
    isMessage = FALSE;
    counter = 0;

    displayLcd(label, LCD0);
    displayLcd(BLANK, LCD1);

    if (currentId != FALSE)
    {
        if (!COIL_UNLOCKED.val)
        {   
            isMessage = TRUE;
			memcpy(lcdLine1,LOCKED,16);
        }
    }

    return nextId;
}


int
onNextMessagePressed(const int nextId, const char * message)
{
    LCD_setcursor(0,0);
    counter = 0;
    isUpdateDisplay = TRUE;
	isMessage = TRUE;
	memcpy(lcdLine1,message,16);
    return nextId;
}


void
displayMnu(const char * mnu, const double fvalue, const int fdigit)
{
         if (fdigit == 0) sprintf(lcdLine1,"%*.0f",16,fvalue); // 0 (integer)
    else if (fdigit == 1) sprintf(lcdLine1,"%*.1f",16,fvalue); // 0.0
    else if (fdigit == 2) sprintf(lcdLine1,"%*.2f",16,fvalue); // 0.00
    else if (fdigit == 3) sprintf(lcdLine1,"%*.3f",16,fvalue); // 0.000
    else if (fdigit == 4) sprintf(lcdLine1,"%*.4f",16,fvalue); // 0.0000
    else if (fdigit == 5) sprintf(lcdLine1,"%*.5f",16,fvalue); // 0.00000

    if (isUpdateDisplay) updateDisplay(mnu,lcdLine1);
    else displayLcd(lcdLine1, LCD1);
}


void
displayFxn(const char * fxn, const double fvalue, const int fdigit)
{
    if (isUpdateDisplay)
    {    

         if (fdigit == 0) sprintf(lcdLine1,"%*.0f",16,fvalue); // 0 (integer)
    else if (fdigit == 1) sprintf(lcdLine1,"%*.1f",16,fvalue); // 0.0
    else if (fdigit == 2) sprintf(lcdLine1,"%*.2f",16,fvalue); // 0.00
    else if (fdigit == 3) sprintf(lcdLine1,"%*.3f",16,fvalue); // 0.000
    else if (fdigit == 4) sprintf(lcdLine1,"%*.4f",16,fvalue); // 0.0000
    else if (fdigit == 5) sprintf(lcdLine1,"%*.5f",16,fvalue); // 0.00000

		updateDisplay(fxn,lcdLine1);
        MENU.col = 16-1;                                 // set cursor right alignment 
        MENU.row = 1;                                               // set line1 
        isUpdateDisplay = FALSE;                                    // disable init
    }    

    LCD_printch(lcdLine1[MENU.col], MENU.col, MENU.row);            // display last char
    LCD_setBlinking(MENU.col,MENU.row);                             // start blinking
}


int
onFxnStepPressed(const int fxnId, const int fdigit)
{
    MENU.col--;
    if (MENU.col <= 16-fdigit) MENU.col = 16-1;   // -000.45'/0' = 8digits
    if (lcdLine1[MENU.col] == '.') MENU.col--;                          // do not alter '.'
    return fxnId;
}


int
onFxnValuePressed(const int fxnId, const BOOL isSigned, const int fdigit)
{
    char val = lcdLine1[MENU.col];
    val++;
   
    // SIGNED
    if (isSigned)
    {
        // SIGNED INTEGER
        if (fdigit == 0)
        {
            if (MENU.col < 16-1)
            {
                if ((val > '9') || (val < '-')) val = '-';
                else if ((val == '/') || (val == '.')) val = '0';   
            }
            else if ((val > '9') || (val < '0')) val = '0';
        }
        // SIGNED FLOAT OR DOUBLE
        else
        {
            if (MENU.col == 16 - (fdigit+1)) val = '.';
            else if (MENU.col < 16-(fdigit+2))
            {
                if ((val > '9') || (val < '-')) val = '-';
                else if ((val == '/') || (val == '.')) val = '0';   
            }
            else if ((val > '9') || (val < '0')) val = '0';
        }
    }	
    // UNSIGNED
    else
    {
        // UNSIGNED INTEGER
        if (fdigit == 0)
        {
            if ((val > '9') || (val < '0')) val = '0';
        }
        // UNSIGNED FLOAT OR DOUBLE
        else
        {
            if (MENU.col == 16 - (fdigit+1)) val = '.';
            else if ((val > '9') || (val < '0')) val = '0';
        }
    }
  
    lcdLine1[MENU.col] = val;

    return fxnId;
}

int
onFxnEnterPressed(const int currentId, const double max, const double min, VAR * vregister, double * dregister, int * iregister)
{
    char val[16] = {0};

    isUpdateDisplay = TRUE;
	isMessage = TRUE;
	int ivalue;
	double dvalue;

   	memcpy(val, lcdLine1, 16);

    ivalue = atoi(val);
    dvalue = atof(val);

    /// INTEGER REGISTER
    if (iregister != NULL_INT)
    {
		if ((*iregister == REG_PASSWORD) && (ivalue == 1343))
		{
			memcpy(lcdLine1,BAD_PASS,16);
    		isUpdateDisplay = FALSE;
    		return currentId;
		}
        else if ((ivalue <= (int)max) && (ivalue >= (int)min))
        {
            *iregister = ivalue;
   	        Swi_post(Swi_writeNand);
			memcpy(lcdLine1,CHANGE_SUCCESS,16);
    		return currentId;
        }
    }
    /// DOUBLE REGISTER 
    else if (dregister != NULL_DBL) 
    {
        if ((dvalue <= max) && (dvalue >= min))
        {
            *dregister = dvalue;
   	        Swi_post(Swi_writeNand);
			memcpy(lcdLine1,CHANGE_SUCCESS,16);
    		return currentId;
        }
    }
	else if (vregister != NULL_VAR) 
    {
        if ((dvalue <= max) && (dvalue >= min))
        {
            VAR_Update(vregister, dvalue, CALC_UNIT);
   	        Swi_post(Swi_writeNand);
			memcpy(lcdLine1,CHANGE_SUCCESS,16);
    		return currentId;
        }
    }
	else
	{
		Swi_post(Swi_writeNand);
		(strcmp(val,CHANGE_SUCCESS) == 0) ? memcpy(lcdLine1,CHANGE_SUCCESS,16) : memcpy(lcdLine1,INVALID, 16);
    	return currentId;
	}

    // INVALID INPUT, STAY IN CURRENT FXN AND RETRY
    isUpdateDisplay = FALSE;
	memcpy(lcdLine1,INVALID,16);

    return currentId;
}

void
changeTime(void)
{
	int index = MENU.col;
	index++;
	if (index == 16) index = 0;
   	if ((index == 2) || (index == 5) || (index == 8) || (index == 11)) index++;
	if ((index == 12) || (index == 13)) index = 14;
	if (index >= 16) index = 0;
	MENU.col = index;
    LCD_printch(lcdLine1[MENU.col], MENU.col, MENU.row);
    LCD_setBlinking(MENU.col, MENU.row);
}

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
////
//// MENU HANDLERS
////
//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////

Uint16 
mnuHomescreenWaterCut(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_WTC;

    static Uint8 isDisplayLogo = 1;
	static Uint8 disp_counter = 0;

    if (isDisplayLogo)
    {
		static Uint8 isInit = 1;
       	static Uint8 logoCounter = 0;

		if (isInit)
		{
			isInit = 0;
			sprintf(lcdLine1, " Razor V%*s ",7,FIRMWARE_VERSION);
			updateDisplay(PHASE_DYNAMICS,lcdLine1);
			isUpdateDisplay = 1;
			sprintf(lcdLine1, "   SN: %0*d",6,REG_SN_PIPE);
		}

		if (isUpdateDisplay && (logoCounter > 10)) updateDisplay(PHASE_DYNAMICS,lcdLine1); 
	    logoCounter++;
		
		if (logoCounter>20) 
		{
			isDisplayLogo = FALSE;
			isUpdateDisplay = 1;
		}

        return MNU_HOMESCREEN_WTC;
    }

	if (isUpdateDisplay) 
	{
		sprintf(lcdLine0,"Watercut %*.2f%%",6,Round_N(REG_WATERCUT.calc_val,2));
		(REG_TEMPERATURE.unit == u_temp_C) ? sprintf(lcdLine1,"Temp %*.1f%cC",9, REG_TEMP_USER.val, LCD_DEGREE) : sprintf(lcdLine1,"Temp %*.1f%cF",9, REG_TEMP_USER.val, LCD_DEGREE);
		updateDisplay(lcdLine0, lcdLine1);
	}
	else if (disp_counter > 10) 
	{
		disp_counter = 0;
		sprintf(lcdLine0,"Watercut %*.2f%%",6,Round_N(REG_WATERCUT.calc_val,2));
		(REG_TEMPERATURE.unit == u_temp_C) ? sprintf(lcdLine1,"Temp %*.1f%cC",9, REG_TEMP_USER.val, LCD_DEGREE) : sprintf(lcdLine1,"Temp %*.1f%cF",9, REG_TEMP_USER.val, LCD_DEGREE);
		updateDisplay(lcdLine0, lcdLine1);
	}
	else disp_counter++;

	switch (input)	{
		case BTN_VALUE 	: return onNextPressed(MNU_HOMESCREEN_FREQ);
		case BTN_STEP 	: return onMnuStepPressed(MNU_OPERATION,FALSE,OPERATION);
		default			: return MNU_HOMESCREEN_WTC;
	}
}


Uint16 
mnuHomescreenFrequency(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_FREQ;

	sprintf(lcdLine1,"%*.3fMhz",13,Round_N(REG_FREQ.calc_val,3));

	(isUpdateDisplay) ? updateDisplay(FREQUENCY, lcdLine1) : (isUpdateDisplay = ~isUpdateDisplay);

	switch (input)  {
        case BTN_VALUE  : return onNextPressed(MNU_HOMESCREEN_RP);
        case BTN_BACK   : return onNextPressed(MNU_HOMESCREEN_WTC);
        default         : return MNU_HOMESCREEN_FREQ;
    }
}


Uint16
mnuHomescreenReflectedPower(const Uint16 input)
{	
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_RP;

	sprintf(lcdLine1,"%*.3fV",15,Round_N(REG_OIL_RP,3));

	(isUpdateDisplay) ? updateDisplay(REFLECTEDPOWER, lcdLine1) : (isUpdateDisplay = ~isUpdateDisplay);

	switch (input)  {
        case BTN_VALUE  : return onNextPressed(MNU_HOMESCREEN_PT);
        case BTN_BACK   : return onNextPressed(MNU_HOMESCREEN_WTC);
        default         : return MNU_HOMESCREEN_RP;
    }
}


Uint16
mnuHomescreenPhaseThreshold(const Uint16 input)
{	
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_PT;

	sprintf(lcdLine1,"%*.3fV",15,Round_N(REG_OIL_PT,3));

	(isUpdateDisplay) ? updateDisplay(PHASETHRESHOLD, lcdLine1) : (isUpdateDisplay = ~isUpdateDisplay);

	switch (input)  {
        case BTN_VALUE  : return onNextPressed(MNU_HOMESCREEN_AVT);
        case BTN_BACK   : return onNextPressed(MNU_HOMESCREEN_WTC);
        default         : return MNU_HOMESCREEN_PT;
    }
}

Uint16
mnuHomescreenAvgTemp(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_AVT;
	static BOOL isEntered = FALSE;
    if (isMessage) { return notifyMessageAndExit(MNU_HOMESCREEN_AVT,MNU_HOMESCREEN_AVT); }

    displayLcd(AVERAGETEMP, LCD0);
	(REG_TEMPERATURE.unit == u_temp_C) ? sprintf(lcdLine1,"%*.1f%cC",14, REG_TEMP_AVG.val, LCD_DEGREE) : sprintf(lcdLine1,"%*.1f%cF",14, REG_TEMP_AVG.val, LCD_DEGREE);
	(isEntered) ? blinkLcdLine1(lcdLine1, STEP_CONFIRM) : blinkLcdLine1(lcdLine1, ENTER_RESET);

	switch (input)	
	{
        case BTN_VALUE  :
			isEntered = FALSE;
			return (REG_OIL_DENS_CORR_MODE == 0) ? onNextPressed(MNU_HOMESCREEN_DGN) : onNextPressed(MNU_HOMESCREEN_DST);
		case BTN_BACK 	: 
			isEntered = FALSE;
            return onNextPressed(MNU_HOMESCREEN_WTC);
		case BTN_ENTER 	:
            if (COIL_UNLOCKED.val) isEntered = TRUE;
			return (COIL_UNLOCKED.val) ? onNextPressed(MNU_HOMESCREEN_AVT) : onNextMessagePressed(MNU_HOMESCREEN_AVT, LOCKED);
		case BTN_STEP	:
			if (!isEntered)	return MNU_HOMESCREEN_AVT;
			isEntered = FALSE;
			COIL_AVGTEMP_RESET.val = TRUE;
			return onNextMessagePressed(MNU_HOMESCREEN_AVT, RESET_SUCCESS);
		default			: return MNU_HOMESCREEN_AVT;
	}
}


Uint16
mnuHomescreenDensity(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_DST;

	static Uint8 index;
	static Uint8 unitLength = 0;

    if (isUpdateDisplay) 
    {
		if (REG_OIL_DENS_CORR_MODE == 1) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_AI, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 2) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MODBUS, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 3) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MANUAL, CALC_UNIT);

       	for (index = 0; index<8; index++)
       	{
           	if (REG_OIL_DENSITY.unit == densityUnit[index]) break;
       	}

		unitLength = strlen(densityIndex[index]);
    }

	sprintf(lcdLine1,"%*.2f%s",16-unitLength,REG_OIL_DENSITY.val,densityIndex[index]);
	(isUpdateDisplay) ? updateDisplay(DENSITY, lcdLine1) : (isUpdateDisplay = ~isUpdateDisplay);

	 switch (input)  {
        case BTN_VALUE  : return onNextPressed(MNU_HOMESCREEN_DGN);
        case BTN_BACK   : return onNextPressed(MNU_HOMESCREEN_WTC);
		case BTN_STEP	: return (REG_OIL_DENS_CORR_MODE == 3) ? onNextPressed(MNU_CFG_DNSCORR_MANUAL) : onNextPressed(MNU_HOMESCREEN_DST);
        default         : return MNU_HOMESCREEN_DST;
    }
}


Uint16
mnuHomescreenDiagnostics(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_DGN;
    static Uint8 i = 0; 
    static Uint8 index = 0; 
    static Uint8 errors[MAX_ERRORS];
    static Uint8 errorCount = 0; 
    static int DIAGNOSTICS_PREV = -1;

    if (isUpdateDisplay || (DIAGNOSTICS != DIAGNOSTICS_PREV))
    {    
        diagnose(&i, &index, &errorCount, errors, &DIAGNOSTICS_PREV);

        if (errorCount > 0) 
        {
            index = errors[i];  // Get error index
            sprintf(lcdLine0,"Diagnostics: %*d",3, errorCount);
            sprintf(lcdLine1,"%*s",16,errorType[index]);
            updateDisplay(lcdLine0,lcdLine1);
        }
        else
        {
            errorCount = 0; 
            sprintf(lcdLine0,"Diagnostics: %*d",3, errorCount);
            memcpy(lcdLine1,BLANK,MAX_LCD_WIDTH);
            updateDisplay(lcdLine0,lcdLine1);
        }
    }    

    switch (input)  {
       case BTN_VALUE  : return onNextPressed(MNU_HOMESCREEN_SRN);
       case BTN_BACK   : return onNextPressed(MNU_HOMESCREEN_WTC);
       case BTN_STEP   : return (DIAGNOSTICS > 0) ? onNextPressed(FXN_HOMESCREEN_DGN) : onNextPressed(MNU_HOMESCREEN_DGN);
       default         : return MNU_HOMESCREEN_DGN;
   }
}


Uint16
fxnHomescreenDiagnostics(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_HOMESCREEN_DGN;
	static Uint8 i = 0;	
	static Uint8 index = 0;
	static Uint8 errors[MAX_ERRORS];
	static Uint8 errorCount = 0;
	static int DIAGNOSTICS_PREV = -1;

	if (DIAGNOSTICS != DIAGNOSTICS_PREV) diagnose(&i, &index, &errorCount, errors, &DIAGNOSTICS_PREV);
	sprintf(lcdLine0,"Diagnostics: %*d",3, errorCount);
	displayLcd(lcdLine0,LCD0);
	index = errors[i];	// Get error index
	displayLcd(errorType[index],LCD1);

	switch (input)	
	{
		case BTN_BACK 	: return onNextPressed(MNU_HOMESCREEN_DGN);
		case BTN_VALUE 	: 
			i++;	
			if (i > errorCount-1) i = 0;
			return FXN_HOMESCREEN_DGN;
		default			: return FXN_HOMESCREEN_DGN;
	}
}


Uint16
mnuHomescreenSerialNumber(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_HOMESCREEN_SRN;

	sprintf(lcdLine1,"%*u",16,(Uint16)REG_SN_PIPE);

	if (isUpdateDisplay) updateDisplay(SERIALNUMBER, lcdLine1);

	switch (input)  {
        case BTN_VALUE  : return onNextPressed(MNU_HOMESCREEN_WTC);
        case BTN_BACK   : return onNextPressed(MNU_HOMESCREEN_WTC);
        default         : return MNU_HOMESCREEN_SRN;
    }
}


// MENU 1.0
Uint16 
mnuOperation(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_OPERATION;

	if (isUpdateDisplay) updateDisplay(OPERATION, BLANK); 

	switch (input)	{
		case BTN_VALUE 	: return onNextPressed(MNU_CFG);
		case BTN_BACK 	: return onNextPressed(MNU_HOMESCREEN_WTC);
		case BTN_STEP 	: return onNextPressed(MNU_OPERATION_STREAM);
		default 		: return MNU_OPERATION;
	}
}


// MENU 2.0
Uint16 
mnuConfig(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG;

	if (isUpdateDisplay) updateDisplay(CONFIGURATION, BLANK); 

	switch (input)	{
		case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO);
		case BTN_STEP 	: return onNextPressed(MNU_CFG_ANALYZER);
		case BTN_BACK 	: return onNextPressed(MNU_HOMESCREEN_WTC);
		default			: return MNU_CFG;
	}
}


// MENU 3.0
Uint16 
mnuSecurityInfo(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO;

	if (isUpdateDisplay) updateDisplay(SECURITYINFO, BLANK); 

	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(MNU_OPERATION);
		case BTN_STEP 	: return onNextPressed(MNU_SECURITYINFO_INFO);
		case BTN_BACK 	: return onNextPressed(MNU_HOMESCREEN_WTC);
		default			: return MNU_SECURITYINFO;
	}
}


// MENU 1.1
Uint16 
mnuOperation_Stream(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_OPERATION_STREAM;

	displayMnu(STREAM,REG_STREAM.calc_val,0);

	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(MNU_OPERATION_OILADJUST);
		case BTN_STEP 	: return onMnuStepPressed(FXN_OPERATION_STREAM,MNU_OPERATION_STREAM, STREAM);
		case BTN_BACK 	: return onNextPressed(MNU_OPERATION);
		default			: return MNU_OPERATION_STREAM;
    }
}


// FXN 1.1 Stream
Uint16 
fxnOperation_Stream(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_OPERATION_STREAM;

    if (isMessage) { return notifyMessageAndExit(FXN_OPERATION_STREAM, MNU_OPERATION_STREAM); }

	displayFxn(STREAM, REG_STREAM.calc_val, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_OPERATION_STREAM,FALSE,0);
        case BTN_STEP   : return onFxnStepPressed(FXN_OPERATION_STREAM,3); // 60'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_OPERATION_STREAM, SMAX, 1.0, &REG_STREAM, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_OPERATION_STREAM);
        default         : return FXN_OPERATION_STREAM;
	}
}


// MENU 1.2 Oil Adjust
Uint16 
mnuOperation_OilAdjust(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_OPERATION_OILADJUST;

    displayMnu(OILADJUST, REG_OIL_ADJUST.calc_val, 2);
	 
	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_OPERATION_OILCAPTURE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_OPERATION_OILADJUST,MNU_OPERATION_OILADJUST, OILADJUST);
		case BTN_BACK 	: return onNextPressed(MNU_OPERATION);
		default			: return MNU_OPERATION_OILADJUST;
	}
}


// FXN 1.2 Oil Adjust
Uint16 
fxnOperation_OilAdjust(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_OPERATION_OILADJUST;

    if (isMessage) { return notifyMessageAndExit(FXN_OPERATION_OILADJUST, MNU_OPERATION_OILADJUST); }

    displayFxn(OILADJUST, REG_OIL_ADJUST.calc_val, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_OPERATION_OILADJUST, TRUE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_OPERATION_OILADJUST,8); // -100.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_OPERATION_OILADJUST, 100.0, -100.0, &REG_OIL_ADJUST, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_OPERATION_OILADJUST);
        default         : return FXN_OPERATION_OILADJUST;
	}
}


// MENU 1.3 Oil Capture
Uint16 
mnuOperation_OilCapture(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_OPERATION_OILCAPTURE;

    COIL_BEGIN_OIL_CAP.val = FALSE;
	(isUpdateDisplay) ? updateDisplay(OILCAPTURE, BLANK) : blinkLcdLine1(STEP_START, BLANK);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_OPERATION_SAMPLE);
		case BTN_BACK 	: return onNextPressed(MNU_OPERATION);
		case BTN_STEP 	: return onMnuStepPressed(FXN_OPERATION_OILCAPTURE,MNU_OPERATION_OILCAPTURE, OILCAPTURE); 
		default			: return MNU_OPERATION_OILCAPTURE;
	}
}


// FXN 1.3 Oil Capture 
Uint16 
fxnOperation_OilCapture(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return FXN_OPERATION_OILCAPTURE;

    static unsigned int blinks = 0; 

    if (isMessage) { return notifyMessageAndExit(FXN_OPERATION_OILCAPTURE, MNU_OPERATION_OILCAPTURE); }

    if (isUpdateDisplay) 
    {
        updateDisplay(OILCAPTURE,BLANK);
        blinks = 0;
        COIL_BEGIN_OIL_CAP.val = TRUE;
        Init_Data_Buffer(); // clear buffer before sampling
    }

	     if (blinks < 1)  blinks += countBlinkTimes(prg0,prg1);
    else if (blinks < 2)  blinks += countBlinkTimes(prg1,prg2);
    else if (blinks < 3)  blinks += countBlinkTimes(prg2,prg3);
    else if (blinks < 4) blinks += countBlinkTimes(prg3,prg4);
    else if (blinks < 5) blinks += countBlinkTimes(prg4,prg5);
    else if (blinks < 6) blinks += countBlinkTimes(prg5,prg6);
    else if (blinks < 7) blinks += countBlinkTimes(prg6,prg7);
    else if (blinks < 8) blinks += countBlinkTimes(prg7,prg8);
    else if (blinks < 9) blinks += countBlinkTimes(prg8,prg9);
    else if (blinks < 10) blinks += countBlinkTimes(prg9,prg10);
    else if (blinks < 11) blinks += countBlinkTimes(prg10,prg11);
    else if (blinks < 12) blinks += countBlinkTimes(prg11,prg12);
    else if (blinks < 13) blinks += countBlinkTimes(prg12,prg13);
    else if (blinks < 14) blinks += countBlinkTimes(prg13,prg14);

    else 
    { 
        COIL_BEGIN_OIL_CAP.val = FALSE;
        return onNextMessagePressed(FXN_OPERATION_OILCAPTURE, STREAM_TIMESTAMP[(int)REG_STREAM.calc_val-1]);
    }

    switch (input)
    {
        // USER STOPS OIL CAPTURING
        case BTN_ENTER  : 
            COIL_BEGIN_OIL_CAP.val = FALSE;
            return onNextMessagePressed(FXN_OPERATION_OILCAPTURE, STREAM_TIMESTAMP[(int)REG_STREAM.calc_val-1]);
        default         : return FXN_OPERATION_OILCAPTURE;
    }
}


// MENU 1.4 Sample
Uint16 
mnuOperation_Sample(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_OPERATION_SAMPLE;

	if (isUpdateDisplay) updateDisplay(SAMPLE, BLANK); 

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_OPERATION_STREAM); // BACK TO MENU 1.1. WE DECIDED TO REMOVE MENU 1.5
		case BTN_STEP 	: return onMnuStepPressed(FXN_OPERATION_SAMPLE_STREAM,MNU_OPERATION_SAMPLE, SAMPLE);
		case BTN_BACK 	: return onNextPressed(MNU_OPERATION);
		default			: return MNU_OPERATION_SAMPLE;
	}
}


// FXN 1.4 Sample - ENTER STREAM
Uint16 
fxnOperation_Sample_Stream(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_OPERATION_SAMPLE_STREAM;

    if (isMessage) { return notifyMessageAndExit(FXN_OPERATION_SAMPLE_STREAM, MNU_OPERATION_SAMPLE); }

    displayFxn(ENTER_STREAM, REG_STREAM.calc_val, 0);

	int val;
	char a[16] = {0};
	memcpy(a,lcdLine1,16);
	val = atoi(a);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_OPERATION_SAMPLE_STREAM,FALSE,0);
        case BTN_STEP   : return onFxnStepPressed(FXN_OPERATION_SAMPLE_STREAM,3); // 60'\0'
        case BTN_ENTER  :
   	        if ((STREAM_TIMESTAMP[val-1][12] != '2') || (val > 60) || (val < 1)) 
			{ 
				return onNextMessagePressed(FXN_OPERATION_SAMPLE_STREAM, INVALID_STREAM);
			}
			else
			{
				TEMP_STREAM = val; 
				return onNextPressed(FXN_OPERATION_SAMPLE_TIMESTAMP);
			}
        case BTN_BACK   : return onFxnBackPressed(FXN_OPERATION_SAMPLE_STREAM);
        default         : return FXN_OPERATION_SAMPLE_STREAM;
	}
}


// FXN 1.4 Sample - Confirm Timestamp 
Uint16 
fxnOperation_Sample_Timestamp(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_OPERATION_SAMPLE_TIMESTAMP;

    if (isMessage) { return notifyMessageAndExit(FXN_OPERATION_SAMPLE_TIMESTAMP, MNU_OPERATION_SAMPLE); }

	if (isUpdateDisplay) updateDisplay(ISTIMECORRECT,STREAM_TIMESTAMP[TEMP_STREAM-1]);

    switch (input)  {
        case BTN_ENTER  : return onNextPressed(FXN_OPERATION_SAMPLE_VALUE);
        case BTN_BACK   : return onFxnBackPressed(FXN_OPERATION_SAMPLE_TIMESTAMP);
        default         : return FXN_OPERATION_SAMPLE_TIMESTAMP;
	}
}

// FXN 1.4 Sample - ENTER VALUE
Uint16 
fxnOperation_Sample_Value(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_OPERATION_SAMPLE_VALUE;

    if (isMessage) { return notifyMessageAndExit(FXN_OPERATION_SAMPLE_VALUE, MNU_OPERATION_SAMPLE); }

    displayFxn(ENTER_VALUE, 0, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_OPERATION_SAMPLE_VALUE, FALSE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_OPERATION_SAMPLE_VALUE,7); // 100.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_OPERATION_SAMPLE_VALUE, 100.0, 0, &REG_OIL_SAMPLE, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_OPERATION_SAMPLE_VALUE);
        default         : return FXN_OPERATION_SAMPLE_VALUE;
	}
}

// MENU 2.1
Uint16 
mnuConfig_Analyzer(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER;
	if (isUpdateDisplay) updateDisplay(CFG_ANALYZER, BLANK); 

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AVGTEMP);
		case BTN_BACK 	: return onNextPressed(MNU_CFG);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_ANALYZER_PROCSAVG, FALSE, CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER;
	}
}


// MENU 2.1.1
Uint16
mnuConfig_Analyzer_ProcsAvg(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_PROCSAVG;

	sprintf(lcdLine1,"%*.0f Samples",8, REG_PROC_AVGING.calc_val);

	if (isUpdateDisplay) updateDisplay(CFG_ANALYZER_PROCSAVG, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_TEMPUNIT);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_PROCSAVG,MNU_CFG_ANALYZER_PROCSAVG, CFG_ANALYZER_PROCSAVG);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_PROCSAVG;
	}
}


// FXN 2.1.1
Uint16
fxnConfig_Analyzer_ProcsAvg(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_PROCSAVG;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_PROCSAVG, MNU_CFG_ANALYZER_PROCSAVG); }

    displayFxn(CFG_ANALYZER_PROCSAVG, REG_PROC_AVGING.calc_val, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_PROCSAVG, FALSE, 0);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_PROCSAVG,3); // 60'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_PROCSAVG, MAXBUF, 1.0, &REG_PROC_AVGING, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_PROCSAVG);
        default         : return FXN_CFG_ANALYZER_PROCSAVG;
	}
}


// MENU 2.1.2
Uint16
mnuConfig_Analyzer_TempUnit(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_TEMPUNIT;

    (REG_TEMPERATURE.unit == u_temp_C) ? sprintf(lcdLine1,"%*cC",15, LCD_DEGREE) : sprintf(lcdLine1,"%*cF",15, LCD_DEGREE);

	if (isUpdateDisplay) updateDisplay(CFG_ANALYZER_TEMPUNIT, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_TEMPADJ);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_TEMPUNIT,MNU_CFG_ANALYZER_TEMPUNIT, CFG_ANALYZER_TEMPUNIT);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_TEMPUNIT;
	}
}


// FXN 2.1.2
Uint16
fxnConfig_Analyzer_TempUnit(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_TEMPUNIT;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_TEMPUNIT, MNU_CFG_ANALYZER_TEMPUNIT); }

	static BOOL isCelsius = TRUE;
   	(isCelsius) ? sprintf(lcdLine1,"%*cC",15, LCD_DEGREE) : sprintf(lcdLine1,"%*cF",15, LCD_DEGREE);
	blinkLcdLine1(lcdLine1, BLANK);

	switch (input)	
	{
		case BTN_VALUE 	:
			isCelsius = !isCelsius;
			counter = 0;
			return FXN_CFG_ANALYZER_TEMPUNIT;
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_TEMPUNIT);
		case BTN_ENTER 	:
			(isCelsius) ? (REG_TEMPERATURE.unit = u_temp_C) : (REG_TEMPERATURE.unit = u_temp_F);
			(isCelsius) ? (REG_TEMP_USER.unit = u_temp_C) : (REG_TEMP_USER.unit = u_temp_F);
			(isCelsius) ? (REG_TEMP_AVG.unit = u_temp_C) : (REG_TEMP_AVG.unit = u_temp_F);
			(isCelsius) ? (REG_TEMP_ADJUST.unit = u_temp_C) : (REG_TEMP_ADJUST.unit = u_temp_F);
			if (REG_TEMP_ADJUST.val != 0) VAR_Update(&REG_TEMP_ADJUST, REG_TEMP_ADJUST.calc_val, CALC_UNIT);
   	        Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_ANALYZER_TEMPUNIT, CHANGE_SUCCESS);
		default			: return FXN_CFG_ANALYZER_TEMPUNIT;
	}
}


// MENU 2.1.3
Uint16
mnuConfig_Analyzer_TempAdj(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_TEMPADJ;

	(REG_TEMPERATURE.unit == u_temp_C) ? sprintf(lcdLine1,"%*.1f%cC",14, REG_TEMP_ADJUST.val, LCD_DEGREE) : sprintf(lcdLine1,"%*.1f%cF",14, REG_TEMP_ADJUST.val, LCD_DEGREE);

	if (isUpdateDisplay) updateDisplay(CFG_ANALYZER_TEMPADJ, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_OILP0);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_TEMPADJ,MNU_CFG_ANALYZER_TEMPADJ,CFG_ANALYZER_TEMPADJ);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_TEMPADJ;
	}
}


// FXN 2.1.3
Uint16
fxnConfig_Analyzer_TempAdj(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_TEMPADJ;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_TEMPADJ, MNU_CFG_ANALYZER_TEMPADJ); }

    displayFxn(CFG_ANALYZER_TEMPADJ, REG_TEMP_ADJUST.val, 1);
	
    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_TEMPADJ, TRUE, 1);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_TEMPADJ,7); // -500.0'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_TEMPADJ, 500.0, -500.0, &REG_TEMP_ADJUST, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_TEMPADJ);
        default         : return FXN_CFG_ANALYZER_TEMPADJ;
	}
}


// MENU 2.1.4
Uint16
mnuConfig_Analyzer_OilP0(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_OILP0;

    displayMnu(CFG_ANALYZER_OILP0, REG_OIL_P0.calc_val, 4);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_OILP1);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_OILP0,MNU_CFG_ANALYZER_OILP0, CFG_ANALYZER_OILP0);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_OILP0;
	}
}


// FXN 2.1.4 
Uint16
fxnConfig_Analyzer_OilP0(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_OILP0;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_OILP0, MNU_CFG_ANALYZER_OILP0); }

    displayFxn(CFG_ANALYZER_OILP0, REG_OIL_P0.calc_val, 4);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_OILP0, TRUE, 4);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_OILP0,10); // -500.0000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_OILP0, 500.0, -500.0, &REG_OIL_P0, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_OILP0);
        default         : return FXN_CFG_ANALYZER_OILP0;
	}
}


// MENU 2.1.5
Uint16
mnuConfig_Analyzer_OilP1(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_OILP1;

    displayMnu(CFG_ANALYZER_OILP1, REG_OIL_P1.calc_val, 4);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_OILINDEX);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_OILP1,MNU_CFG_ANALYZER_OILP1, CFG_ANALYZER_OILP1);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_OILP1;
	}
}


// FXN 2.1.5
Uint16
fxnConfig_Analyzer_OilP1(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_OILP1;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_OILP1, MNU_CFG_ANALYZER_OILP1); }

    displayFxn(CFG_ANALYZER_OILP1, REG_OIL_P1.calc_val, 4);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_OILP1, TRUE, 4);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_OILP1,10); // -500.0000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_OILP1, 500.0, -500.0, &REG_OIL_P1, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_OILP1);
        default         : return FXN_CFG_ANALYZER_OILP1;
	}
}

// MENU 2.1.6
Uint16
mnuConfig_Analyzer_OilIndex(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_OILINDEX;

    sprintf(lcdLine1,"%*.3fMhz",13, Round_N(REG_OIL_INDEX.calc_val,3));

    if (isUpdateDisplay) updateDisplay(CFG_ANALYZER_OILINDEX, lcdLine1);

    switch (input)  
    {    
        case BTN_VALUE  : return onNextPressed(MNU_CFG_ANALYZER_OILFREQLOW);
        case BTN_STEP   : return onMnuStepPressed(FXN_CFG_ANALYZER_OILINDEX,MNU_CFG_ANALYZER_OILINDEX, CFG_ANALYZER_OILINDEX);
        case BTN_BACK   : return onNextPressed(MNU_CFG_ANALYZER);
        default         : return MNU_CFG_ANALYZER_OILINDEX;
    } 
}


// FXN 2.1.6
Uint16
fxnConfig_Analyzer_OilIndex(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_OILINDEX;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_OILINDEX, MNU_CFG_ANALYZER_OILINDEX); }

    displayFxn(CFG_ANALYZER_OILINDEX, REG_OIL_INDEX.calc_val, 3);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_OILINDEX,TRUE, 3);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_OILINDEX,10); // -1000.900'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_OILINDEX, 1000.0, -1000.0, &REG_OIL_INDEX, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_OILINDEX);
        default         : return FXN_CFG_ANALYZER_OILINDEX;
	}
}


// MENU 2.1.7
Uint16
mnuConfig_Analyzer_OilFreqLow(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_OILFREQLOW;

    sprintf(lcdLine1,"%*.3fMhz",13,Round_N(REG_OIL_FREQ_LOW.calc_val,3));

    if (isUpdateDisplay) updateDisplay(CFG_ANALYZER_OILFREQLOW, lcdLine1);
	
	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_OILFREQHI);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_OILFREQLOW,MNU_CFG_ANALYZER_OILFREQLOW, CFG_ANALYZER_OILFREQLOW);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_OILFREQLOW;
	}
}


// FXN 2.1.7
Uint16
fxnConfig_Analyzer_OilFreqLow(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_OILFREQLOW;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_OILFREQLOW, MNU_CFG_ANALYZER_OILFREQLOW); }

    displayFxn(CFG_ANALYZER_OILFREQLOW, REG_OIL_FREQ_LOW.calc_val, 3);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_OILFREQLOW, FALSE, 3);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_OILFREQLOW,9); // 1000.000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_OILFREQLOW,1000.0, 0, &REG_OIL_FREQ_LOW, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_OILFREQLOW);
        default         : return FXN_CFG_ANALYZER_OILFREQLOW;
	}
}


// MENU 2.1.8
Uint16
mnuConfig_Analyzer_OilFreqHi(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_OILFREQHI;

	sprintf(lcdLine1,"%*.3fMhz",13,Round_N(REG_OIL_FREQ_HIGH.calc_val,3));

    if (isUpdateDisplay) updateDisplay(CFG_ANALYZER_OILFREQHI, lcdLine1);
	
	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_PHASEHOLD);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_OILFREQHI,MNU_CFG_ANALYZER_OILFREQHI, CFG_ANALYZER_OILFREQHI);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_OILFREQHI;
	}
}


// FXN 2.1.8
Uint16
fxnConfig_Analyzer_OilFreqHi(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_OILFREQHI;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_OILFREQHI, MNU_CFG_ANALYZER_OILFREQHI); }

    displayFxn(CFG_ANALYZER_OILFREQHI, REG_OIL_FREQ_HIGH.calc_val, 3);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_OILFREQHI, FALSE, 3);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_OILFREQHI,9); // 1000.000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_OILFREQHI, 1000.0, 0, &REG_OIL_FREQ_HIGH, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_OILFREQHI);
        default         : return FXN_CFG_ANALYZER_OILFREQHI;
	}
}


// MENU 2.1.9
Uint16
mnuConfig_Analyzer_PhaseHold(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_ANALYZER_PHASEHOLD;
    
    displayMnu(CFG_ANALYZER_PHASEHOLD,(double)REG_PHASE_HOLD_CYCLES, 0);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER_PROCSAVG);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_ANALYZER_PHASEHOLD,MNU_CFG_ANALYZER_PHASEHOLD, CFG_ANALYZER_PHASEHOLD);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_ANALYZER);
		default			: return MNU_CFG_ANALYZER_PHASEHOLD;
    }
}


// FXN 2.1.9
Uint16
fxnConfig_Analyzer_PhaseHold(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_ANALYZER_PHASEHOLD;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_ANALYZER_PHASEHOLD, MNU_CFG_ANALYZER_PHASEHOLD); }

    displayFxn(CFG_ANALYZER_PHASEHOLD, (double)REG_PHASE_HOLD_CYCLES, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_ANALYZER_PHASEHOLD, FALSE, 0);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_ANALYZER_PHASEHOLD,3); // 60'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_ANALYZER_PHASEHOLD, 60.0, 0, NULL_VAR, NULL_DBL, &REG_PHASE_HOLD_CYCLES);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_ANALYZER_PHASEHOLD);
        default         : return FXN_CFG_ANALYZER_PHASEHOLD;
	}
}


// MENU 2.2
Uint16 
mnuConfig_AvgTemp(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AVGTEMP;

	(REG_TEMPERATURE.unit == u_temp_C) ? sprintf(lcdLine1,"%*.1f%cC",14, REG_TEMP_AVG.val, LCD_DEGREE) : sprintf(lcdLine1,"%*.1f%cF",14, REG_TEMP_AVG.val, LCD_DEGREE);
	(isUpdateDisplay) ? updateDisplay(CFG_AVGTEMP, lcdLine1) : displayLcd(lcdLine1,LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DATALOGGER);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_AVGTEMP_MODE,FALSE, CFG_AVGTEMP);
		case BTN_BACK 	: return onNextPressed(MNU_CFG);
		default			: return MNU_CFG_AVGTEMP;
	}
}


// MENU 2.2.1
Uint16 
mnuConfig_AvgTemp_Mode(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AVGTEMP_MODE;

    if (isUpdateDisplay) (COIL_AVGTEMP_MODE.val) ? updateDisplay(CFG_AVGTEMP_MODE,TWENTYFOURHR) : updateDisplay(CFG_AVGTEMP_MODE,ONDEMAND);
		
	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AVGTEMP_AVGRESET);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AVGTEMP_MODE,MNU_CFG_AVGTEMP_MODE, CFG_AVGTEMP_MODE);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AVGTEMP);
		default			: return MNU_CFG_AVGTEMP_MODE;
	}
}


// FXN 2.2.1
Uint16 
fxnConfig_AvgTemp_Mode(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AVGTEMP_MODE;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AVGTEMP_MODE, MNU_CFG_AVGTEMP_MODE); }

	static Uint8 index = 0;
	(index) ? blinkLcdLine1(TWENTYFOURHR, BLANK) : blinkLcdLine1(ONDEMAND, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			index++;
			if (index > 1) index = 0;
			return FXN_CFG_AVGTEMP_MODE;
        case BTN_ENTER  : 
			COIL_AVGTEMP_MODE.val = index; 
            Swi_post(Swi_writeNand);
			index = 0;
			return onNextMessagePressed(FXN_CFG_AVGTEMP_MODE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AVGTEMP_MODE);
        default         : return FXN_CFG_AVGTEMP_MODE;
	}
}


// MENU 2.2.2
Uint16 
mnuConfig_AvgTemp_AvgReset(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AVGTEMP_AVGRESET;

	if (isUpdateDisplay) updateDisplay(CFG_AVGTEMP_AVGRESET, BLANK); 

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AVGTEMP_MODE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AVGTEMP_AVGRESET,MNU_CFG_AVGTEMP_AVGRESET, CFG_AVGTEMP_AVGRESET);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AVGTEMP);
		default			: return MNU_CFG_AVGTEMP_AVGRESET;
    }
}


// FXN 2.2.2
Uint16 
fxnConfig_AvgTemp_AvgReset(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AVGTEMP_AVGRESET;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AVGTEMP_AVGRESET, MNU_CFG_AVGTEMP_AVGRESET); }

	static BOOL isEntered = FALSE;
	(isEntered) ? blinkLcdLine1(STEP_CONFIRM, BLANK) : blinkLcdLine1(ENTER_RESET, BLANK);

	switch (input)	
	{
		case BTN_BACK 	:
			isEntered = FALSE;
            return onFxnBackPressed(FXN_CFG_AVGTEMP_AVGRESET);
		case BTN_ENTER 	:
			counter = 0;
			isEntered = TRUE;
    		return FXN_CFG_AVGTEMP_AVGRESET;
		case BTN_STEP	:
			if (!isEntered) return FXN_CFG_AVGTEMP_AVGRESET;
			counter = 0;
			isEntered = FALSE;
			COIL_AVGTEMP_RESET.val = TRUE;
			return onNextMessagePressed(FXN_CFG_AVGTEMP_AVGRESET, RESET_SUCCESS);
		default			: return FXN_CFG_AVGTEMP_AVGRESET;
	}
}


// MENU 2.3
Uint16 
mnuConfig_DataLogger(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DATALOGGER;
	if (isUpdateDisplay) updateDisplay(CFG_DATALOGGER, BLANK);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AO);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_DATALOGGER_ENABLELOGGER,FALSE, CFG_DATALOGGER);
		case BTN_BACK 	: return onNextPressed(MNU_CFG);
		default			: return MNU_CFG_DATALOGGER;
	}
}


// MENU 2.3.1
Uint16 
mnuConfig_DataLogger_EnableLogger(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DATALOGGER_ENABLELOGGER;

	/* keep tracking current usb status */
	static int status_prev = 0;

	if (status_prev != usbStatus) 
	{
		isUpdateDisplay = TRUE; 
		status_prev = usbStatus;
	}

	if (isUpdateDisplay) updateDisplay(CFG_DATALOGGER_ENABLELOGGER,USB_CODE[usbStatus]);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DATALOGGER_PERIOD);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DATALOGGER_ENABLELOGGER,MNU_CFG_DATALOGGER_ENABLELOGGER, CFG_DATALOGGER_ENABLELOGGER);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DATALOGGER);
		default			: return MNU_CFG_DATALOGGER_ENABLELOGGER;
	}
}


// FXN 2.3.1
Uint16 
fxnConfig_DataLogger_EnableLogger(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return FXN_CFG_DATALOGGER_ENABLELOGGER;

    static BOOL isEnabled = FALSE;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DATALOGGER_ENABLELOGGER, MNU_CFG_DATALOGGER_ENABLELOGGER); }

    if (isUpdateDisplay)
    {
        (usbStatus == 1) ? (isEnabled = 1) : (isEnabled = 0);

		if (!isEnabled)
		{
			/* display LCD0 */
			updateDisplay(CFG_DATALOGGER_ENABLELOGGER, MOUNTING_USB);
			usb_osalDelayMs(200);

	    	/* enumerate usb */
    		Swi_post( Swi_usbhMscDriveOpen );
    		Swi_post( Swi_enumerateUsb );
			(isUsbMounted) ? displayLcd(USB_MOUNTED,LCD1) : displayLcd(USB_UNMOUNTED, LCD1);
			usb_osalDelayMs(1000);
		}

		isUpdateDisplay = FALSE;
	}

	/* usb drive is not deteced */
	if (!isUsbMounted) return onFxnBackPressed(FXN_CFG_DATALOGGER_ENABLELOGGER);

	/* the current state is the first option to choose from */
    (isEnabled) ? blinkLcdLine1(ENABLE, BLANK) : blinkLcdLine1(DISABLE, BLANK);

    switch (input)  {
        case BTN_VALUE  :
            counter = 0; 
            isEnabled = !isEnabled;
            return FXN_CFG_DATALOGGER_ENABLELOGGER;
        case BTN_ENTER  : 
			isLogData = isEnabled;
			if (isLogData) usbStatus = 1;
            else
            {
				resetUsbStaticVars();
                usbStatus = 0;
            }
            return onNextMessagePressed(FXN_CFG_DATALOGGER_ENABLELOGGER,CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DATALOGGER_ENABLELOGGER);
        default         : return FXN_CFG_DATALOGGER_ENABLELOGGER;
    }    
}


// MENU 2.3.2
Uint16 
mnuConfig_DataLogger_Period(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DATALOGGER_PERIOD;

	sprintf(lcdLine1, "%*d Secs",11, REG_LOGGING_PERIOD);
	if (isUpdateDisplay) updateDisplay(CFG_DATALOGGER_PERIOD, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DATALOGGER_ENABLELOGGER);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DATALOGGER_PERIOD,MNU_CFG_DATALOGGER_PERIOD, CFG_DATALOGGER_PERIOD);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DATALOGGER);
		default			: return MNU_CFG_DATALOGGER_PERIOD;
	}
}


// FXN 2.3.2
Uint16 
fxnConfig_DataLogger_Period(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DATALOGGER_PERIOD;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DATALOGGER_PERIOD, MNU_CFG_DATALOGGER_PERIOD); }
    displayFxn(CFG_DATALOGGER_PERIOD, REG_LOGGING_PERIOD, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DATALOGGER_PERIOD, FALSE, 0);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DATALOGGER_PERIOD,5); // 3600'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DATALOGGER_PERIOD, 59.0, 2.0, NULL_VAR, NULL_DBL, &REG_LOGGING_PERIOD);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DATALOGGER_PERIOD);
        default         : return FXN_CFG_DATALOGGER_PERIOD;
	}
}


// MENU 2.4 
Uint16 
mnuConfig_AnalogOutput(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO;

	sprintf(lcdLine1, "%*.2fmA",14, Round_N(REG_AO_OUTPUT,1));

	if (isUpdateDisplay) updateDisplay(CFG_AO, lcdLine1); 
	displayLcd(lcdLine1,LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_COMM);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_AO_LRV,FALSE,CFG_AO);
		case BTN_BACK 	: return onNextPressed(MNU_CFG);
		default			: return MNU_CFG_AO;
	}
}

// MENU 2.4.1
Uint16 
mnuConfig_AO_LRV(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_LRV;

	sprintf(lcdLine1, "%*.2f%%",15, REG_AO_LRV.calc_val);

	if (isUpdateDisplay) updateDisplay(CFG_AO_LRV, lcdLine1);
	
	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AO_URV);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_LRV,MNU_CFG_AO_LRV, CFG_AO_LRV);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_LRV;
	}
}


// FXN 2.4.1
Uint16 
fxnConfig_AO_LRV(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_LRV;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AO_LRV, MNU_CFG_AO_LRV); }
    displayFxn(CFG_AO_LRV, REG_AO_LRV.calc_val, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_AO_LRV, FALSE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_AO_LRV,7); // 100.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_AO_LRV, 100.0, 0, &REG_AO_LRV, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AO_LRV);
        default         : return FXN_CFG_AO_LRV;
	}
}


// MENU 2.4.2
Uint16 
mnuConfig_AO_URV(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_URV;

	sprintf(lcdLine1, "%*.2f%%",15, REG_AO_URV.calc_val);
	if (isUpdateDisplay) updateDisplay(CFG_AO_URV, lcdLine1);
	
	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AO_DAMPENING);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_URV,MNU_CFG_AO_URV, CFG_AO_URV);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_URV;
	}
}


// FXN 2.4.2
Uint16 
fxnConfig_AO_URV(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_URV;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AO_URV, MNU_CFG_AO_URV); }
    displayFxn(CFG_AO_URV, REG_AO_URV.calc_val, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_AO_URV,FALSE,2);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_AO_URV,7);
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_AO_URV, 100.0, 0, &REG_AO_URV, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AO_URV);
        default         : return FXN_CFG_AO_URV;
	}
}


// MENU 2.4.3
Uint16 
mnuConfig_AO_Dampening(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_DAMPENING;

    sprintf(lcdLine1, "%*dSecs",12, REG_AO_DAMPEN);
	if (isUpdateDisplay) updateDisplay(CFG_AO_DAMPENING, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AO_ALARM);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_DAMPENING,MNU_CFG_AO_DAMPENING, CFG_AO_DAMPENING);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_DAMPENING;
	}
}


// FXN 2.4.3
Uint16 
fxnConfig_AO_Dampening(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_DAMPENING;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AO_DAMPENING, MNU_CFG_AO_DAMPENING); }
    displayFxn(CFG_AO_DAMPENING, (double)REG_AO_DAMPEN, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_AO_DAMPENING, FALSE,0);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_AO_DAMPENING,3); // 60'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_AO_DAMPENING, 60.0, 0, NULL_VAR, NULL_DBL, &REG_AO_DAMPEN);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AO_DAMPENING);
        default         : return FXN_CFG_AO_DAMPENING;
	}
}


// MENU 2.4.4
Uint16 
mnuConfig_AO_Alarm(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_ALARM;

    sprintf(lcdLine1,"%*s",16,errorMode[REG_AO_ALARM_MODE]);
    (isUpdateDisplay) ? updateDisplay(CFG_AO_ALARM, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_AO_TRIMLO);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_ALARM,MNU_CFG_AO_ALARM, CFG_AO_ALARM);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_ALARM;
	}
}


// FXN 2.4.4 
Uint16 
fxnConfig_AO_Alarm(const Uint16 input)
{	
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_ALARM;

	static Uint8 index;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AO_ALARM, MNU_CFG_AO_ALARM); }

	strcpy(lcdLine1, errorMode[index]); 
	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			index++;
			if (index > 2) index = 0;
			return FXN_CFG_AO_ALARM;
        case BTN_ENTER  : 
			REG_AO_ALARM_MODE = index;
   	        Swi_post(Swi_writeNand);
            return onNextMessagePressed(FXN_CFG_AO_ALARM, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AO_ALARM);
        default         : return FXN_CFG_AO_ALARM;
	}
}


// MENU 2.4.5
Uint16 
mnuConfig_AO_TrimLo(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_TRIMLO;

    static double manualValLoPrev = 0;      // holds current manual output value
	static Uint8 aoModeLoPrev 	= 0;        // holds current MANUAL mode status
	static BOOL isSaveValue 		= TRUE;
    COIL_AO_TRIM_MODE.val = FALSE;

    // SAVE CURRENT AO SETTINGS
    if (isSaveValue)
    {
        aoModeLoPrev = REG_AO_MODE;
        manualValLoPrev = REG_AO_MANUAL_VAL;
        isSaveValue = FALSE;
    }

    if (isUpdateDisplay)
    {
  	    sprintf (lcdLine1, "%*.3fmA",14, REG_AO_TRIMLO);
        updateDisplay(CFG_AO_TRIMLO, lcdLine1);
    }

	switch (input)	
	{
		case BTN_VALUE 	: 
            isSaveValue = TRUE;
            REG_AO_MODE = aoModeLoPrev;
			REG_AO_MANUAL_VAL = manualValLoPrev; 
   	        Swi_post(Swi_writeNand);
            return onNextPressed(MNU_CFG_AO_TRIMHI);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_TRIMLO,MNU_CFG_AO_TRIMLO,CFG_AO_TRIMLO);
		case BTN_BACK 	: 
            isSaveValue = TRUE;
            REG_AO_MODE = aoModeLoPrev;
			REG_AO_MANUAL_VAL = manualValLoPrev; 
   	        Swi_post(Swi_writeNand);
            return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_TRIMLO;
	}
}


// FXN 2.4.5
Uint16
fxnConfig_AO_TrimLo(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_TRIMLO;

    static double manualValLoFxnPrev = 0;    // holds current manual output value
	static Uint8 aoModeLoFxnPrev 	= 0;    // holds current MANUAL mode status
	static BOOL isInitTrim 		= TRUE;
	COIL_AO_TRIM_MODE.val 		= TRUE;

    if (isMessage) return notifyMessageAndExit(FXN_CFG_AO_TRIMLO, MNU_CFG_AO_TRIMLO);
	
    displayFxn(CFG_MEASVAL, REG_AO_TRIMLO, 3);

	if (isInitTrim)
	{
		aoModeLoFxnPrev = REG_AO_MODE; 			// save current AO mode
		manualValLoFxnPrev = REG_AO_MANUAL_VAL; // save current AO manual val
		REG_AO_MODE = 2;                        // MANUAL
		REG_AO_MANUAL_VAL = 4.00;               // FORCE 4 mA via MANUAL
		isInitTrim = FALSE;
	}

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_AO_TRIMLO, FALSE, 3);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_AO_TRIMLO,7); // 30.000'\0'
        case BTN_ENTER  : 
			isInitTrim  = TRUE;
			return onFxnEnterPressed(FXN_CFG_AO_TRIMLO, 30.0, 0, NULL_VAR, &REG_AO_TRIMLO, NULL_INT);
        case BTN_BACK   : 
			isInitTrim  = TRUE;
			REG_AO_MODE = aoModeLoFxnPrev;
			REG_AO_MANUAL_VAL = manualValLoFxnPrev;
   	        Swi_post(Swi_writeNand);
			return onFxnBackPressed(FXN_CFG_AO_TRIMLO);
        default         : return FXN_CFG_AO_TRIMLO;
	}
}


// MENU 2.4.6
Uint16 
mnuConfig_AO_TrimHi(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_TRIMHI;

    static double manualValHiPrev = 0;    // holds current manual output value
	static Uint8 aoModeHiPrev 	= 0;    // holds current MANUAL mode status
	static BOOL isSaveValue 		= TRUE;
    COIL_AO_TRIM_MODE.val = FALSE;

    // SAVE CURRENT AO SETTINGS
    if (isSaveValue)
    {
        aoModeHiPrev = REG_AO_MODE;
        manualValHiPrev = REG_AO_MANUAL_VAL;
        isSaveValue = FALSE;
    }

    if (isUpdateDisplay)
    {
	    sprintf (lcdLine1,"%*.3fmA",14,REG_AO_TRIMHI);
        updateDisplay(CFG_AO_TRIMHI, lcdLine1);
    }

	switch (input)	
	{
		case BTN_VALUE 	: 
            isSaveValue = TRUE;
            REG_AO_MODE = aoModeHiPrev;
			REG_AO_MANUAL_VAL = manualValHiPrev; 
   	        Swi_post(Swi_writeNand);
            return onNextPressed(MNU_CFG_AO_MODE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_TRIMHI, MNU_CFG_AO_TRIMHI, CFG_AO_TRIMHI);
		case BTN_BACK 	: 
            isSaveValue = TRUE;
            REG_AO_MODE = aoModeHiPrev;
			REG_AO_MANUAL_VAL = manualValHiPrev; 
   	        Swi_post(Swi_writeNand);
            return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_TRIMHI;
	}
}


// FXN 2.4.6
Uint16 
fxnConfig_AO_TrimHi(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_TRIMHI;

    static double manualValHiFxnPrev = 0;    // holds current manual output value
	static Uint8 aoModeHiFxnPrev 	= 0;    // holds current MANUAL mode status
	static BOOL isInitTrim 		= TRUE;
	COIL_AO_TRIM_MODE.val 		= TRUE;

    if (isMessage) return notifyMessageAndExit(FXN_CFG_AO_TRIMHI, MNU_CFG_AO_TRIMHI);
	
    displayFxn(CFG_MEASVAL, REG_AO_TRIMHI, 3);

	if (isInitTrim)
	{
		aoModeHiFxnPrev = REG_AO_MODE; 			// save current AO mode
		manualValHiFxnPrev = REG_AO_MANUAL_VAL; 	// save current AO manual val
		REG_AO_MODE = 2;
		REG_AO_MANUAL_VAL = 20.00;
		isInitTrim = FALSE;
	}

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_AO_TRIMHI, FALSE,3);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_AO_TRIMHI,7); // 30.000'\0'
        case BTN_ENTER  : 
			isInitTrim  = TRUE;
			return onFxnEnterPressed(FXN_CFG_AO_TRIMHI, 30.0, 0, NULL_VAR, &REG_AO_TRIMHI, NULL_INT);
        case BTN_BACK   : 
			isInitTrim  = TRUE;
			REG_AO_MODE = aoModeHiFxnPrev;
			REG_AO_MANUAL_VAL = manualValHiFxnPrev;
   	        Swi_post(Swi_writeNand);
			return onFxnBackPressed(FXN_CFG_AO_TRIMHI);
        default         : return FXN_CFG_AO_TRIMHI;
	}
}


// MENU 2.4.7
Uint16 
mnuConfig_AO_Mode(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_MODE;

    strcpy(lcdLine1, aoMode[REG_AO_MODE]);

    (isUpdateDisplay) ? updateDisplay(CFG_AO_MODE, lcdLine1) : displayLcd(lcdLine1,LCD1);

	switch (input)	
	{
        case BTN_VALUE 	:
			isUpdateDisplay = TRUE;
			return (REG_AO_MODE == 2) ? MNU_CFG_AO_AOVALUE : MNU_CFG_AO_LRV;
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_MODE,MNU_CFG_AO_MODE, CFG_AO_MODE);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_MODE;
	}
}


// FXN 2.4.7
Uint16 
fxnConfig_AO_Mode(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_MODE;

	static Uint8 index;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AO_MODE, MNU_CFG_AO_MODE); }

    sprintf(lcdLine1,"%*s",16,aoMode[index]); 
    blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			index++;
			if (index > 2) index = 0;
			return FXN_CFG_AO_MODE;
        case BTN_ENTER  : 
			REG_AO_MODE = index; 
   	        Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_AO_MODE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AO_MODE);
        default         : return FXN_CFG_AO_MODE;
	}
}


// MENU 2.4.8
Uint16 
mnuConfig_AO_AoValue(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_AO_AOVALUE;
    sprintf(lcdLine1, "%*.2fmA",14, REG_AO_MANUAL_VAL);

    (isUpdateDisplay) ? updateDisplay(CFG_AO_AOVALUE, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(MNU_CFG_AO_LRV);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_AO_AOVALUE,MNU_CFG_AO_AOVALUE, CFG_AO_AOVALUE);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_AO);
		default			: return MNU_CFG_AO_AOVALUE;
	}
}


// FXN 2.4.8
Uint16 
fxnConfig_AO_AoValue(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_AO_AOVALUE;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_AO_AOVALUE, MNU_CFG_AO_AOVALUE); }

    displayFxn(CFG_AO_AOVALUE, REG_AO_MANUAL_VAL, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_AO_AOVALUE, FALSE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_AO_AOVALUE,6); // 22.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_AO_AOVALUE, 22.0, 2.0, NULL_VAR, &REG_AO_MANUAL_VAL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_AO_AOVALUE);
        default         : return FXN_CFG_AO_AOVALUE;
	}
}


// MENU 2.5
Uint16 
mnuConfig_Comm(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_COMM;
	(isUpdateDisplay) ? updateDisplay(CFG_COMM, BLANK) : displayLcd(BLANK,LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_RELAY);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_COMM_SLAVEADDR,FALSE, CFG_COMM);
		case BTN_BACK 	: return onNextPressed(MNU_CFG);
		default			: return MNU_CFG_COMM;
	}
}


// MENU 2.5.1
Uint16 
mnuConfig_Comm_SlaveAddr(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_COMM_SLAVEADDR;

    sprintf(lcdLine1, "%*d",16, REG_SLAVE_ADDRESS);
    (isUpdateDisplay) ? updateDisplay(CFG_COMM_SLAVEADDR, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_COMM_BAUDRATE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_COMM_SLAVEADDR,MNU_CFG_COMM_SLAVEADDR, CFG_COMM_SLAVEADDR);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_COMM);
		default			: return MNU_CFG_COMM_SLAVEADDR;
	}
}


// FXN 2.5.1
Uint16 
fxnConfig_Comm_SlaveAddr(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_COMM_SLAVEADDR;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_COMM_SLAVEADDR, MNU_CFG_COMM_SLAVEADDR); }
    
    displayFxn(CFG_COMM_SLAVEADDR, REG_SLAVE_ADDRESS, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_COMM_SLAVEADDR,FALSE,0);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_COMM_SLAVEADDR,4); // 247'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_COMM_SLAVEADDR,247, 0, NULL_VAR, NULL_DBL, &REG_SLAVE_ADDRESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_COMM_SLAVEADDR);
        default         : return FXN_CFG_COMM_SLAVEADDR;
	}
}


// MENU 2.5.2
Uint16 
mnuConfig_Comm_BaudRate(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_COMM_BAUDRATE;

	sprintf(lcdLine1,"%*dBPS",13,(Uint32)REG_BAUD_RATE.calc_val);

	if (isUpdateDisplay) updateDisplay(CFG_COMM_BAUDRATE, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_COMM_PARITY);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_COMM_BAUDRATE,MNU_CFG_COMM_BAUDRATE, CFG_COMM_BAUDRATE);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_COMM);
		default			: return MNU_CFG_COMM_BAUDRATE;
	}
}


// FXN 2.5.2
Uint16 
fxnConfig_Comm_BaudRate(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_COMM_BAUDRATE;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_COMM_BAUDRATE, MNU_CFG_COMM_BAUDRATE); }

	static Uint8 index;
	const double baudrate[7] = {2400.0, 4800.0, 9600.0, 19200.0, 38400.0, 57600.0, 115200.0};

	sprintf(lcdLine1,"%*.0f",16, baudrate[index]);
	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			counter = 0;
			index++;
			if (index > 6) index = 0;
			return FXN_CFG_COMM_BAUDRATE;
        case BTN_ENTER  :
			VAR_Update(&REG_BAUD_RATE, baudrate[index], CALC_UNIT);
			Config_Uart((Uint32)REG_BAUD_RATE.calc_val, COIL_PARITY.val);
   	        Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_COMM_BAUDRATE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_COMM_BAUDRATE);
        default         : return FXN_CFG_COMM_BAUDRATE;
    }
}


// MENU 2.5.3
Uint16 
mnuConfig_Comm_Parity(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_COMM_PARITY;

	if (isUpdateDisplay) (COIL_PARITY.val) ? updateDisplay(CFG_COMM_PARITY, EVEN) : updateDisplay(CFG_COMM_PARITY, NONE);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_COMM_STATISTICS);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_COMM_PARITY,MNU_CFG_COMM_PARITY, CFG_COMM_PARITY);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_COMM);
		default			: return MNU_CFG_COMM_PARITY;
	}
}


// FXN 2.5.3
Uint16 
fxnConfig_Comm_Parity(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_COMM_PARITY;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_COMM_PARITY, MNU_CFG_COMM_PARITY); }

	static BOOL isEnabled = TRUE;
	(isEnabled) ? blinkLcdLine1(EVEN, BLANK) : blinkLcdLine1(NONE, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			counter = 0;
			isEnabled = !isEnabled;
			return FXN_CFG_COMM_PARITY;
        case BTN_ENTER  : 
			COIL_PARITY.val = isEnabled; 
            Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_COMM_PARITY, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_COMM_PARITY);
        default         : return FXN_CFG_COMM_PARITY;
	}
}


// MENU 2.5.4
Uint16 
mnuConfig_Comm_Statistics(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_COMM_STATISTICS;

         if (REG_STATISTICS == 0) sprintf(lcdLine1, "Success:%*d",8,STAT_SUCCESS);
    else if (REG_STATISTICS == 1) sprintf(lcdLine1, "Inv Pkt:%*d",8,STAT_PKT);
    else if (REG_STATISTICS == 2) sprintf(lcdLine1, "Inv Cmd:%*d",8,STAT_CMD);
    else if (REG_STATISTICS == 3) sprintf(lcdLine1, "Retry:%*d",10,STAT_RETRY);
    else
    {
	         if (STAT_CURRENT == 0) sprintf(lcdLine1, "Success:%*d",8,STAT_SUCCESS);
	    else if (STAT_CURRENT == 1) sprintf(lcdLine1, "Inv Pkt:%*d",8,STAT_PKT);
	    else if (STAT_CURRENT == 2) sprintf(lcdLine1, "Inv Cmd:%*d",8,STAT_CMD);
	    else sprintf(lcdLine1, "Retry:%*d",10,STAT_RETRY);
    }

    if (isUpdateDisplay) updateDisplay(CFG_COMM_STATISTICS, lcdLine1); 
    else displayLcd(lcdLine1,LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_COMM_SLAVEADDR);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_COMM_STATISTICS,FALSE,CFG_COMM_STATISTICS);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_COMM);
		default			: return MNU_CFG_COMM_STATISTICS;
	}
}


// FXN 2.5.4
Uint16 
fxnConfig_Comm_Statistics(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_COMM_STATISTICS;
    
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_COMM_STATISTICS, MNU_CFG_COMM_STATISTICS); }

	static Uint8 index;
	const char * statMode[5] = {ST_SUCCESS, ST_PKT, ST_CMD, ST_RETRY, AUTOMATIC}; 

	sprintf(lcdLine1,"%*s",16,statMode[index]); 
	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			counter = 0;
			index++;
			if (index > 4) index = 0;
			return FXN_CFG_COMM_STATISTICS;
        case BTN_ENTER  :
			REG_STATISTICS = index; 
   	        Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_COMM_STATISTICS, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_COMM_STATISTICS);
        default         : return FXN_CFG_COMM_STATISTICS;
    }
}


// MENU 2.6
Uint16 
mnuConfig_Relay(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_RELAY;

    (COIL_RELAY[0].val) ? sprintf(lcdLine1,"%*s",16,RELAY_ON) : sprintf(lcdLine1,"%*s",16,RELAY_OFF);
    (isUpdateDisplay) ? updateDisplay(CFG_RELAY, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_RELAY_DELAY,FALSE,CFG_RELAY);
		case BTN_BACK 	: return onNextPressed(MNU_CFG);
		default			: return MNU_CFG_RELAY;
	}
}


// MENU 2.6.1
Uint16 
mnuConfig_Relay_Delay(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_RELAY_DELAY;

    sprintf(lcdLine1, "%*dSecs",12, REG_RELAY_DELAY);

    (isUpdateDisplay) ? updateDisplay(CFG_RELAY_DELAY, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_RELAY_MODE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_RELAY_DELAY,MNU_CFG_RELAY_DELAY,CFG_RELAY_DELAY);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_RELAY);
		default			: return MNU_CFG_RELAY_DELAY;
	}
}


// FXN 2.6.1
Uint16 
fxnConfig_Relay_Delay(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_RELAY_DELAY;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_RELAY_DELAY, MNU_CFG_RELAY_DELAY); }

    displayFxn(CFG_RELAY_DELAY, (double)REG_RELAY_DELAY, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_RELAY_DELAY, FALSE,0);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_RELAY_DELAY,3); // 60'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_RELAY_DELAY, 60, 0, NULL_VAR, NULL_DBL, &REG_RELAY_DELAY);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_RELAY_DELAY);
        default         : return FXN_CFG_RELAY_DELAY;
	}
}


// MENU 2.6.2
Uint16 
mnuConfig_Relay_Mode(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_RELAY_MODE;

	memcpy(lcdLine1,relayMode[REG_RELAY_MODE],16);

    (isUpdateDisplay) ? updateDisplay(CFG_RELAY_MODE, lcdLine1) : displayLcd(lcdLine1, LCD1);
	
	switch (input)	
	{
		case BTN_VALUE 	:
			isUpdateDisplay = TRUE;
			if (REG_RELAY_MODE == 0) return MNU_CFG_RELAY_SETPOINT;
			else if (REG_RELAY_MODE == 1) return MNU_CFG_RELAY_ACTWHILE;
			else if (REG_RELAY_MODE == 3) return MNU_CFG_RELAY_RELAYSTATUS;
			else return MNU_CFG_RELAY_DELAY; // 2_6_3 does not exist
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_RELAY_MODE,MNU_CFG_RELAY_MODE,CFG_RELAY_MODE);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_RELAY);
		default			: return MNU_CFG_RELAY_MODE;
	}
}


// FXN 2.6.2
Uint16 
fxnConfig_Relay_Mode(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_RELAY_MODE;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_RELAY_MODE, MNU_CFG_RELAY_MODE); }
	static Uint8 index;
	memcpy(lcdLine1,relayMode[index],16);
	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			index++;
			if (index > 3) index = 0;
			return FXN_CFG_RELAY_MODE;
        case BTN_ENTER  : 
			REG_RELAY_MODE = index; 
   	        Swi_post(Swi_writeNand);
            return onNextMessagePressed(FXN_CFG_RELAY_MODE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_RELAY_MODE);
        default         : return FXN_CFG_RELAY_MODE;
	}
}


// MENU 2.6.3
Uint16 
mnuConfig_Relay_ActWhile(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_RELAY_ACTWHILE;

	memcpy(lcdLine1,phaseMode[COIL_ACT_RELAY_OIL.val],16);
	(isUpdateDisplay) ? updateDisplay(CFG_RELAY_VAR,lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_RELAY_DELAY);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_RELAY_ACTWHILE,MNU_CFG_RELAY_ACTWHILE,CFG_RELAY_VAR);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_RELAY);
		default			: return MNU_CFG_RELAY_ACTWHILE;
	}
}


// FXN 2.6.3
Uint16 
fxnConfig_Relay_ActWhile(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_RELAY_ACTWHILE;

    static Uint8 index;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_RELAY_ACTWHILE, MNU_CFG_RELAY_ACTWHILE); }

	memcpy(lcdLine1,phaseMode[index],16);
    blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
            index++;
            if (index > 1) index = 0;
			return FXN_CFG_RELAY_ACTWHILE;
        case BTN_ENTER  :  
            COIL_ACT_RELAY_OIL.val = index;
            Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_RELAY_ACTWHILE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_RELAY_ACTWHILE);
        default         : return FXN_CFG_RELAY_ACTWHILE;
    }
}


// MENU 2.6.3
Uint16 
mnuConfig_Relay_RelayStatus(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_RELAY_RELAYSTATUS;

	memcpy(lcdLine1,statusMode[COIL_RELAY_MANUAL.val],16);

	(isUpdateDisplay) ? updateDisplay(CFG_RELAY_RELAYSTATUS,lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_RELAY_DELAY);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_RELAY_RELAYSTATUS,MNU_CFG_RELAY_RELAYSTATUS,CFG_RELAY_RELAYSTATUS);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_RELAY);
		default			: return MNU_CFG_RELAY_RELAYSTATUS;
	}
}


// FXN 2.6.3
Uint16 
fxnConfig_Relay_RelayStatus(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_RELAY_RELAYSTATUS;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_RELAY_RELAYSTATUS, MNU_CFG_RELAY_RELAYSTATUS); }
	const char * statusMode[2] = {RELAY_OFF, RELAY_ON}; 
    static Uint8 index;
	memcpy(lcdLine1,statusMode[index],16);
    blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
            index++;
            if (index > 1) index = 0;
			return FXN_CFG_RELAY_RELAYSTATUS;
        case BTN_ENTER  :  
            COIL_RELAY_MANUAL.val = index;
            Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_RELAY_RELAYSTATUS, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_RELAY_RELAYSTATUS);
        default         : return FXN_CFG_RELAY_RELAYSTATUS;
    }
}


// MENU 2.6.3
Uint16 
mnuConfig_Relay_SetPoint(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_RELAY_SETPOINT;

    sprintf(lcdLine1, "%*.1f%%",15, Round_N(REG_RELAY_SETPOINT.calc_val,1));

    (isUpdateDisplay) ? updateDisplay(CFG_RELAY_SETPOINT,lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_RELAY_DELAY);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_RELAY_SETPOINT,MNU_CFG_RELAY_SETPOINT,CFG_RELAY_SETPOINT);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_RELAY);
		default			: return MNU_CFG_RELAY_SETPOINT;
	}
}


// FXN 2.6.3
Uint16 
fxnConfig_Relay_SetPoint(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_RELAY_SETPOINT;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_RELAY_SETPOINT, MNU_CFG_RELAY_SETPOINT); }
    
    displayFxn(CFG_RELAY_SETPOINT, REG_RELAY_SETPOINT.calc_val, 1);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_RELAY_SETPOINT, TRUE,1);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_RELAY_SETPOINT,9); // -10000.9'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_RELAY_SETPOINT, 10000.0, -10000.0, &REG_RELAY_SETPOINT, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_RELAY_SETPOINT);
        default         : return FXN_CFG_RELAY_SETPOINT;
	}
}


// MENU 2.7
Uint16 
mnuConfig_DnsCorr(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR;

	sprintf(lcdLine1,"%*s",16,densityMode[REG_OIL_DENS_CORR_MODE]);

	if (isUpdateDisplay)
    {
        if (REG_OIL_DENS_CORR_MODE == 1) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_AI, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 2) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MODBUS, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 3) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MANUAL, CALC_UNIT);

        updateDisplay(CFG_DNSCORR, lcdLine1);
    }
    else displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_ANALYZER);
		case BTN_STEP 	: return onMnuStepPressed(MNU_CFG_DNSCORR_CORRENABLE,FALSE,CFG_DNSCORR);
       	case BTN_BACK 	: return onNextPressed(MNU_CFG);
		default			: return MNU_CFG_DNSCORR;
	}
}


// MENU 2.7.1
Uint16 
mnuConfig_DnsCorr_CorrEnable(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_CORRENABLE;

	sprintf(lcdLine1,"%*s",16,densityMode[REG_OIL_DENS_CORR_MODE]);

	if (isUpdateDisplay)
    {
        if (REG_OIL_DENS_CORR_MODE == 1) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_AI, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 2) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MODBUS, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 3) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MANUAL, CALC_UNIT);
		else DIAGNOSTICS &= ~(ERR_DNS_LO | ERR_DNS_HI);

		updateDisplay(CFG_DNSCORR_CORRENABLE, lcdLine1);
    }
    else displayLcd(lcdLine1, LCD1);

	switch (input)
	{
		case BTN_VALUE  : return onNextPressed(MNU_CFG_DNSCORR_DISPUNIT);
		case BTN_STEP   : return onMnuStepPressed(FXN_CFG_DNSCORR_CORRENABLE,MNU_CFG_DNSCORR_CORRENABLE,CFG_DNSCORR_CORRENABLE);
		case BTN_BACK   : return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_CORRENABLE;
	}
}


// FXN 2.7.1
Uint16 
fxnConfig_DnsCorr_CorrEnable(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_CORRENABLE;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_CORRENABLE, MNU_CFG_DNSCORR_CORRENABLE); }

	static Uint8 index;

	memcpy(lcdLine1,densityMode[index],MAX_LCD_WIDTH);

	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			index++;
			if (index > 3) index = 0;
			return FXN_CFG_DNSCORR_CORRENABLE;
        case BTN_ENTER  :
			REG_OIL_DENS_CORR_MODE = index;
   	        Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_DNSCORR_CORRENABLE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_CORRENABLE);
        default         : return FXN_CFG_DNSCORR_CORRENABLE;
	}
}


// MENU 2.7.2
Uint16 
mnuConfig_DnsCorr_DispUnit(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_DISPUNIT;

	static Uint8 index;

    if (isUpdateDisplay)
    {
		if (REG_OIL_DENS_CORR_MODE == 1) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_AI, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 2) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MODBUS, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 3) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MANUAL, CALC_UNIT);

        for (index = 0; index<8; index++)
        {
            if (REG_OIL_DENSITY.unit == densityUnit[index]) break;
        }

		sprintf(lcdLine1,"%*s",16,densityIndex[index]);
	    updateDisplay(CFG_DNSCORR_DISPUNIT, lcdLine1);
    }

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_COEFD0);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_DISPUNIT,MNU_CFG_DNSCORR_DISPUNIT,CFG_DNSCORR_DISPUNIT);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_DISPUNIT;
	}
}


// FXN 2.7.2
Uint16 
fxnConfig_DnsCorr_DispUnit(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_DISPUNIT;
    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_DISPUNIT, MNU_CFG_DNSCORR_DISPUNIT); }

	static Uint8 index;

    if (isUpdateDisplay)
    {
		if (REG_OIL_DENS_CORR_MODE == 1) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_AI, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 2) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MODBUS, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 3) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MANUAL, CALC_UNIT);

        for (index = 0; index<8; index++)
        {
            if (REG_OIL_DENSITY.unit == densityUnit[index]) break;
        }

		sprintf(lcdLine1,"%*s",16,densityIndex[index]);
	    updateDisplay(CFG_DNSCORR_DISPUNIT, lcdLine1);
    }

	sprintf(lcdLine1,"%*s",16,densityIndex[index]);
	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			(index < 7) ? (index++) : (index = 0);
			return FXN_CFG_DNSCORR_DISPUNIT;
        case BTN_ENTER  : 
			REG_OIL_DENSITY.unit = densityUnit[index];
			VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY.calc_val, CALC_UNIT);
   	       	Swi_post(Swi_writeNand);
            return onNextMessagePressed(FXN_CFG_DNSCORR_DISPUNIT, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_DISPUNIT);
        default         : return FXN_CFG_DNSCORR_DISPUNIT;
	}
}


// MENU 2.7.3
Uint16 
mnuConfig_DnsCorr_CoefD0(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_COEFD0;
    
	displayMnu(CFG_DNSCORR_COEFD0, REG_DENSITY_D0.calc_val, 4);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_COEFD1);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_COEFD0,MNU_CFG_DNSCORR_COEFD0,CFG_DNSCORR_COEFD0);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_COEFD0;
	}
}


// FXN 2.7.3
Uint16 
fxnConfig_DnsCorr_CoefD0(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_COEFD0;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_COEFD0, MNU_CFG_DNSCORR_COEFD0); }

    displayFxn(CFG_DNSCORR_COEFD0, REG_DENSITY_D0.calc_val, 4);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_COEFD0, TRUE, 4);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_COEFD0,12); // -10000.0000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_COEFD0, 10000.0, -10000.0, &REG_DENSITY_D0, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_COEFD0);
        default         : return FXN_CFG_DNSCORR_COEFD0;
	}
}


// MENU 2.7.4
Uint16 
mnuConfig_DnsCorr_CoefD1(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_COEFD1;
    
	displayMnu(CFG_DNSCORR_COEFD1, REG_DENSITY_D1.calc_val, 4);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_COEFD2);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_COEFD1,MNU_CFG_DNSCORR_COEFD1,CFG_DNSCORR_COEFD1);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_COEFD1;
	}
}


// FXN 2.7.4
Uint16 
fxnConfig_DnsCorr_CoefD1(const Uint16 input)
{	
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_COEFD1;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_COEFD1, MNU_CFG_DNSCORR_COEFD1); }

    displayFxn(CFG_DNSCORR_COEFD1, REG_DENSITY_D1.calc_val, 4);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_COEFD1, TRUE, 4);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_COEFD1,12); // -10000.0000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_COEFD1, 10000.0, -10000.0, &REG_DENSITY_D1, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_COEFD1);
        default         : return FXN_CFG_DNSCORR_COEFD1;
	}
}


// MENU 2.7.5
Uint16 
mnuConfig_DnsCorr_CoefD2(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_COEFD2;

    displayMnu(CFG_DNSCORR_COEFD2, REG_DENSITY_D2.calc_val, 4);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_COEFD3);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_COEFD2,MNU_CFG_DNSCORR_COEFD2,CFG_DNSCORR_COEFD2);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_COEFD2;
	}
}


// FXN 2.7.5
Uint16 
fxnConfig_DnsCorr_CoefD2(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_COEFD2; 

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_COEFD2, MNU_CFG_DNSCORR_COEFD2); }

    displayFxn(CFG_DNSCORR_COEFD2, REG_DENSITY_D2.calc_val, 4);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_COEFD2, TRUE, 4);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_COEFD2,12); // -10000.0000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_COEFD2, 10000.0, -10000.0, &REG_DENSITY_D2, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_COEFD2);
        default         : return FXN_CFG_DNSCORR_COEFD2;
	}	
}


// MENU 2.7.6
Uint16 
mnuConfig_DnsCorr_CoefD3(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_COEFD3;

    displayMnu(CFG_DNSCORR_COEFD3, REG_DENSITY_D3.calc_val, 4);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_INPUTUNIT);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_COEFD3,MNU_CFG_DNSCORR_COEFD3,CFG_DNSCORR_COEFD3);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_COEFD3;
	}
}


// FXN 2.7.6
Uint16 
fxnConfig_DnsCorr_CoefD3(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_COEFD3;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_COEFD3, MNU_CFG_DNSCORR_COEFD3); }

    displayFxn(CFG_DNSCORR_COEFD3, REG_DENSITY_D3.calc_val, 4);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_COEFD3, TRUE, 4);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_COEFD3,12); // -10000.0000'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_COEFD3, 10000.0, -10000.0, &REG_DENSITY_D3, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_COEFD3);
        default         : return FXN_CFG_DNSCORR_COEFD3;
	}
}


// MENU 2.7.7
Uint16 
mnuConfig_DnsCorr_InputUnit(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_INPUTUNIT;

	static Uint8 index;

    if (isUpdateDisplay)
    {
		if (REG_OIL_DENS_CORR_MODE == 1) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_AI, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 2) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MODBUS, CALC_UNIT);
        else if (REG_OIL_DENS_CORR_MODE == 3) VAR_Update(&REG_OIL_DENSITY, REG_OIL_DENSITY_MANUAL, CALC_UNIT);

        for (index = 0; index<8; index++)
        {
            if (REG_OIL_DENSITY.calc_unit == densityUnit[index]) break;
        }

		sprintf(lcdLine1,"%*s",16,densityIndex[index]);
	    updateDisplay(CFG_DNSCORR_INPUTUNIT, lcdLine1);
    }

	switch (input)	
	{
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_INPUTUNIT,MNU_CFG_DNSCORR_INPUTUNIT,CFG_DNSCORR_INPUTUNIT);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		case BTN_VALUE 	:
			isUpdateDisplay = TRUE;
			if (REG_OIL_DENS_CORR_MODE == 1) return MNU_CFG_DNSCORR_AILRV;			// Analog Input
			else if (REG_OIL_DENS_CORR_MODE == 3) return MNU_CFG_DNSCORR_MANUAL;	// Manual
			else return MNU_CFG_DNSCORR_CORRENABLE;
		default			: return MNU_CFG_DNSCORR_INPUTUNIT;
    }
}


// FXN 2.7.7
Uint16 
fxnConfig_DnsCorr_InputUnit(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_INPUTUNIT;

	double tempDensityVal = 0;
	double tempLrvVal = 0;
	double tempUrvVal = 0;

	static Uint8 index;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_INPUTUNIT, MNU_CFG_DNSCORR_INPUTUNIT); }

    if (isUpdateDisplay)
    {
        for (index = 0; index<8; index++)
        {
            if (REG_OIL_DENSITY.calc_unit == densityUnit[index]) break;
        }

		sprintf(lcdLine1,"%*s",16,densityIndex[index]);
	    updateDisplay(CFG_DNSCORR_INPUTUNIT, lcdLine1);
    }

	sprintf(lcdLine1,"%*s",16,densityIndex[index]);
	blinkLcdLine1(lcdLine1, BLANK);

    switch (input)  {
        case BTN_VALUE  :
			(index<7) ? (index++) : (index = 0);
			return FXN_CFG_DNSCORR_INPUTUNIT;
        case BTN_ENTER  : 
			tempDensityVal = REG_OIL_DENSITY.calc_val;
			tempLrvVal = REG_OIL_DENSITY_AI_LRV.calc_val;
			tempUrvVal = REG_OIL_DENSITY_AI_URV.calc_val;
			REG_OIL_DENSITY.calc_unit = densityUnit[index];
			REG_OIL_DENSITY_AI_LRV.calc_unit = REG_OIL_DENSITY.calc_unit; 
			REG_OIL_DENSITY_AI_URV.calc_unit = REG_OIL_DENSITY.calc_unit;
			VAR_Update(&REG_OIL_DENSITY, tempDensityVal, CALC_UNIT);
			VAR_Update(&REG_OIL_DENSITY_AI_LRV, tempLrvVal, CALC_UNIT);
			VAR_Update(&REG_OIL_DENSITY_AI_URV, tempUrvVal, CALC_UNIT);
   	        Swi_post(Swi_writeNand);
			return onNextMessagePressed(FXN_CFG_DNSCORR_INPUTUNIT, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_INPUTUNIT);
        default         : return FXN_CFG_DNSCORR_INPUTUNIT;
	}
}


// MENU 2.7.8
Uint16 
mnuConfig_DnsCorr_Manual(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_MANUAL;
    
	displayMnu(CFG_DNSCORR_MANUAL, REG_OIL_DENSITY_MANUAL, 2);

	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_CORRENABLE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_MANUAL,MNU_CFG_DNSCORR_MANUAL,CFG_DNSCORR_MANUAL);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_MANUAL;
	}
}


// FXN 2.7.8
Uint16 
fxnConfig_DnsCorr_Manual(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_MANUAL;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_MANUAL, MNU_CFG_DNSCORR_MANUAL); }

    displayFxn(CFG_DNSCORR_MANUAL, REG_OIL_DENSITY_MANUAL, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_MANUAL, FALSE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_MANUAL,10); // -10000.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_MANUAL, 10000.0, 0, NULL_VAR, &REG_OIL_DENSITY_MANUAL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_MANUAL);
        default         : return FXN_CFG_DNSCORR_MANUAL;
	}
}


// MENU 2.7.8
Uint16 
mnuConfig_DnsCorr_AiLrv(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_AILRV;

	sprintf(lcdLine1,"%*.2f",16,REG_OIL_DENSITY_AI_LRV.calc_val);
    if (isUpdateDisplay) updateDisplay(CFG_DNSCORR_AILRV, lcdLine1);

    displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_AIURV);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_AILRV,MNU_CFG_DNSCORR_AILRV,CFG_DNSCORR_AILRV);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_AILRV;
	}
}


// FXN 2.7.8
Uint16 
fxnConfig_DnsCorr_AiLrv(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_AILRV;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_AILRV, MNU_CFG_DNSCORR_AILRV); }

    displayFxn(CFG_DNSCORR_AILRV, REG_OIL_DENSITY_AI_LRV.calc_val, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_AILRV, FALSE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_AILRV,10); // 100000.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_AILRV, 100000.0, 0, &REG_OIL_DENSITY_AI_LRV, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_AILRV);
        default         : return FXN_CFG_DNSCORR_AILRV;
	}
}


// MENU 2.7.9
Uint16 
mnuConfig_DnsCorr_AiUrv(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_AIURV;
	sprintf(lcdLine1,"%*.2f",16, REG_OIL_DENSITY_AI_URV.calc_val);
    if (isUpdateDisplay) updateDisplay(CFG_DNSCORR_AIURV, lcdLine1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_AI_TRIMLO);
		case BTN_STEP 	: return onMnuStepPressed(FXN_CFG_DNSCORR_AIURV,MNU_CFG_DNSCORR_AIURV,CFG_DNSCORR_AIURV);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		default			: return MNU_CFG_DNSCORR_AIURV;
	}
}


// FXN 2.7.9
Uint16 
fxnConfig_DnsCorr_AiUrv(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_AIURV;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_AIURV, MNU_CFG_DNSCORR_AIURV); }

    displayFxn(CFG_DNSCORR_AIURV, REG_OIL_DENSITY_AI_URV.calc_val, 2);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_CFG_DNSCORR_AIURV, FALSE, 2);
        case BTN_STEP   : return onFxnStepPressed(FXN_CFG_DNSCORR_AIURV,10); // 100000.00'\0'
        case BTN_ENTER  : return onFxnEnterPressed(FXN_CFG_DNSCORR_AIURV, 100000.0, 0, &REG_OIL_DENSITY_AI_URV, NULL_DBL, NULL_INT);
        case BTN_BACK   : return onFxnBackPressed(FXN_CFG_DNSCORR_AIURV);
        default         : return FXN_CFG_DNSCORR_AIURV;
	}
}


// MENU 2.7.10
Uint16 
mnuConfig_DnsCorr_Ai_TrimLo(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_AI_TRIMLO;

    COIL_AI_TRIM_MODE.val = FALSE;
    sprintf(lcdLine1,"%*.4fmA",14,REG_AI_TRIMMED);

    (isUpdateDisplay) ? updateDisplay(CFG_DNSCORR_AI_TRIMLO,lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_AI_TRIMHI);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		case BTN_STEP 	:
			COIL_AI_TRIM_MODE.val = TRUE;
			return onMnuStepPressed(FXN_CFG_DNSCORR_AI_TRIMLO,MNU_CFG_DNSCORR_AI_TRIMLO,CFG_DNSCORR_AI_TRIMLO);
		default			: return MNU_CFG_DNSCORR_AI_TRIMLO;
	}
}

// FXN 2.7.10
Uint16 
fxnConfig_DnsCorr_Ai_TrimLo(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_AI_TRIMLO;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_AI_TRIMLO, MNU_CFG_DNSCORR_AI_TRIMLO); }

	sprintf(lcdLine1,"%*.4fmA",14, REG_AI_MEASURE);
	(isUpdateDisplay) ? updateDisplay(CFG_MEASVAL, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
		case BTN_ENTER 	:
			REG_AI_TRIMLO = REG_AI_MEASURE;
   	        Swi_post(Swi_writeNand);
			COIL_AI_TRIM_MODE.val = FALSE;
			return onNextMessagePressed(FXN_CFG_DNSCORR_AI_TRIMLO, CHANGE_SUCCESS);
		case BTN_BACK 	:
			COIL_AI_TRIM_MODE.val = FALSE;
            return onFxnBackPressed(FXN_CFG_DNSCORR_AI_TRIMLO);
		default			: return FXN_CFG_DNSCORR_AI_TRIMLO;
	}
}


// MENU 2.7.11
Uint16 
mnuConfig_DnsCorr_Ai_TrimHi(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_CFG_DNSCORR_AI_TRIMHI;

    COIL_AI_TRIM_MODE.val = FALSE;

    sprintf(lcdLine1,"%*.4fmA",14, REG_AI_TRIMMED);
    (isUpdateDisplay) ?  updateDisplay(CFG_DNSCORR_AI_TRIMHI,lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_CFG_DNSCORR_CORRENABLE);
		case BTN_BACK 	: return onNextPressed(MNU_CFG_DNSCORR);
		case BTN_STEP 	:
			COIL_AI_TRIM_MODE.val = TRUE;
			return onMnuStepPressed(FXN_CFG_DNSCORR_AI_TRIMHI,MNU_CFG_DNSCORR_AI_TRIMHI,CFG_DNSCORR_AI_TRIMHI);
		default			: return MNU_CFG_DNSCORR_AI_TRIMHI;
	}
}


// FXN 2.7.11
Uint16 
fxnConfig_DnsCorr_Ai_TrimHi(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_CFG_DNSCORR_AI_TRIMHI;

    if (isMessage) { return notifyMessageAndExit(FXN_CFG_DNSCORR_AI_TRIMHI, MNU_CFG_DNSCORR_AI_TRIMHI); }

	sprintf(lcdLine1,"%*.4fmA",14,REG_AI_MEASURE);
	(isUpdateDisplay) ? updateDisplay(CFG_MEASVAL, lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
		case BTN_ENTER 	:
			REG_AI_TRIMHI = REG_AI_MEASURE;
   	        Swi_post(Swi_writeNand);
			COIL_AI_TRIM_MODE.val = FALSE;
			return onNextMessagePressed(FXN_CFG_DNSCORR_AI_TRIMHI, CHANGE_SUCCESS);
		case BTN_BACK 	:
			COIL_AI_TRIM_MODE.val = FALSE;
            return onFxnBackPressed(FXN_CFG_DNSCORR_AI_TRIMHI);
		default			: return FXN_CFG_DNSCORR_AI_TRIMHI;
	}
}


// MENU 3.1
Uint16 
mnuSecurityInfo_Info(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_INFO;

   	(isUpdateDisplay) ? updateDisplay(SECURITYINFO_INFO,BLANK) : displayLcd(BLANK, LCD1);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_TIMEANDDATE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_SN,FALSE,SECURITYINFO_INFO);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_INFO;
	}
}


// MENU 3.1.1
Uint16 
fxnSecurityInfo_SN(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_SN;

    sprintf(lcdLine1,"%*u",16,(Uint16) REG_SN_PIPE);
	if (isUpdateDisplay) updateDisplay(SECURITYINFO_INFO_SN, lcdLine1);
	
	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(FXN_SECURITYINFO_MC);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO_INFO); 
		default			: return FXN_SECURITYINFO_SN;
	}
}


// MENU 3.1.2
Uint16 
fxnSecurityInfo_MC(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_MC;

	if (isUpdateDisplay)
	{
		Uint8 i;
	    char lcdModelCode[] = "INCDYNAMICSPHASE";

		for (i=0;i<4;i++)
        {
            lcdModelCode[i*4+3] = (REG_MODEL_CODE[i] >> 24) & 0xFF;
            lcdModelCode[i*4+2] = (REG_MODEL_CODE[i] >> 16) & 0xFF;
            lcdModelCode[i*4+1] = (REG_MODEL_CODE[i] >> 8)  & 0xFF;
            lcdModelCode[i*4+0] = (REG_MODEL_CODE[i] >> 0)  & 0xFF;
        }
       
        sprintf(lcdLine1,"%*s",16,lcdModelCode);
        updateDisplay(SECURITYINFO_INFO_MC, lcdLine1);
	}
	
    switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(FXN_SECURITYINFO_FW);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO_INFO); 
		default			: return FXN_SECURITYINFO_MC;
	}
}

// MENU 3.1.3
Uint16 
fxnSecurityInfo_FW(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_FW;

	sprintf(lcdLine1,"%*s",16,FIRMWARE_VERSION);
	if (isUpdateDisplay) updateDisplay(SECURITYINFO_INFO_FW, lcdLine1);

    switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(FXN_SECURITYINFO_HW);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO_INFO); 
		default			: return FXN_SECURITYINFO_FW;
	}
}

// MENU 3.1.4
Uint16 
fxnSecurityInfo_HW(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_HW;

	sprintf(lcdLine1,"%*s",16,HARDWARE_VERSION);
	if (isUpdateDisplay) updateDisplay(SECURITYINFO_INFO_HW, lcdLine1);

    switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(FXN_SECURITYINFO_SN);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO_INFO); 
		default			: return FXN_SECURITYINFO_HW;
	}
}


// MENU 3.2
Uint16 
mnuSecurityInfo_TimeAndDate(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_TIMEANDDATE;
    sprintf(lcdLine1,"%02u:%02u %02u/%02u/20%02u",REG_RTC_HR,REG_RTC_MIN,REG_RTC_MON,REG_RTC_DAY,REG_RTC_YR);

    (isUpdateDisplay) ? updateDisplay(SECURITYINFO_TIMEANDDATE,lcdLine1) : displayLcd(lcdLine1, LCD1);

	switch (input)	
	{
		case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_ACCESSTECH);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_TIMEANDDATE,MNU_SECURITYINFO_TIMEANDDATE,SECURITYINFO_TIMEANDDATE);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_TIMEANDDATE;
	}
}


// FXN 3.2
Uint16 
fxnSecurityInfo_TimeAndDate(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_TIMEANDDATE;
    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_TIMEANDDATE, MNU_SECURITYINFO_TIMEANDDATE); }

    char hh[2], mn[2], mm[2], dd[2], yy[2];

    if (isUpdateDisplay)
    {
		isUpdateDisplay = FALSE;
		displayLcd(SECURITYINFO_TIMEANDDATE,LCD0);
        sprintf(lcdLine1,"%02u:%02u %02u/%02u/20%02u",REG_RTC_HR,REG_RTC_MIN,REG_RTC_MON,REG_RTC_DAY,REG_RTC_YR);
		displayLcd(lcdLine1,LCD1);
        MENU.col = 0; 
        MENU.row = 1; 
        LCD_printch(lcdLine1[MENU.col], MENU.col, MENU.row);
        LCD_setBlinking(MENU.col,MENU.row);
    }

	switch (input)  {
        case BTN_VALUE  :
            lcdLine1[MENU.col]+=0x01;
			     if ((MENU.col == 0) && (lcdLine1[MENU.col] > '2'))                                     lcdLine1[MENU.col] = '0';  // H_:__ __/__/____
			else if ((MENU.col == 1) && (lcdLine1[MENU.col-1] == '2') && (lcdLine1[MENU.col] > '3'))    lcdLine1[MENU.col] = '0';  // _H:__ __/__/____
			else if ((MENU.col == 2))                                                                   lcdLine1[MENU.col] = ':';
			else if ((MENU.col == 3) && (lcdLine1[MENU.col] > '5'))                                     lcdLine1[MENU.col] = '0';  // __:M_ __/__/____
			else if ((MENU.col == 5))                                                                   lcdLine1[MENU.col] = 0x20;
			else if ((MENU.col == 6) && (lcdLine1[MENU.col+1] > '2'))                                   lcdLine1[MENU.col] = '0';  // __:__ M_/__/____
			else if ((MENU.col == 6) && (lcdLine1[MENU.col] > '1'))                                     lcdLine1[MENU.col] = '0';  // __:__ M_/__/____
            else if ((MENU.col == 7) && (lcdLine1[MENU.col-1] == '1') && (lcdLine1[MENU.col] > '2'))    lcdLine1[MENU.col] = '0';  // __:__ _M/__/____
			else if ((MENU.col == 8) || (MENU.col == 11))                                               lcdLine1[MENU.col] = '/';  
			else if ((MENU.col == 9) && (lcdLine1[MENU.col] > '2') && (lcdLine1[MENU.col+1] > '1') )    lcdLine1[MENU.col] = '0';  // __:__ __/D_/____ 
			else if ((MENU.col == 9) && (lcdLine1[MENU.col] > '3'))                                     lcdLine1[MENU.col] = '0';  // __:__ __/D_/____ 
            else if ((MENU.col == 10) && (lcdLine1[MENU.col-1] > '2') && (lcdLine1[MENU.col] > '1'))    lcdLine1[MENU.col] = '0';  // __:__ __/_D/____
			else if ((MENU.col == 12))                                                                  lcdLine1[MENU.col] = '2';  // __:__ __/__/2___
			else if ((MENU.col == 13))                                                                  lcdLine1[MENU.col] = '0';  // __:__ __/__/_0__
            else if ((lcdLine1[MENU.col] > '9') || (lcdLine1[MENU.col] < '0'))                          lcdLine1[MENU.col] = '0';  // __:__ __/__/__YY
            LCD_printch(lcdLine1[MENU.col], MENU.col, MENU.row);
            LCD_setBlinking(MENU.col, MENU.row);
			return FXN_SECURITYINFO_TIMEANDDATE;
        case BTN_STEP   :	
            /*MENU.col++;
			if (MENU.col == 16) MENU.col = 0;
           	if ((MENU.col == 2) || (MENU.col == 5) || (MENU.col == 8) || (MENU.col == 11)) MENU.col++;
			if ((MENU.col == 12) || (MENU.col == 13)) MENU.col = 14;
			if (MENU.col >= 16) MENU.col = 0;
            LCD_printch(lcdLine1[MENU.col], MENU.col, MENU.row);
            LCD_setBlinking(MENU.col, MENU.row);*/
			Swi_post(Swi_changeTime);
			return FXN_SECURITYINFO_TIMEANDDATE;
        case BTN_ENTER  :
        	REG_RTC_SEC_IN = 0;
			sprintf(hh,"%c%c",lcdLine1[0],lcdLine1[1]);
        	REG_RTC_HR_IN = atoi(hh);
			sprintf(mn,"%c%c",lcdLine1[3],lcdLine1[4]);
        	REG_RTC_MIN_IN = atoi(mn);
			sprintf(mm,"%c%c",lcdLine1[6],lcdLine1[7]);
        	REG_RTC_MON_IN = atoi(mm);
			sprintf(dd,"%c%c",lcdLine1[9],lcdLine1[10]);
        	REG_RTC_DAY_IN = atoi(dd);
			sprintf(yy,"%c%c",lcdLine1[14],lcdLine1[15]);
        	REG_RTC_YR_IN = atoi(yy);
            isWriteRTC = TRUE;	 // RTC trigger
			return onNextMessagePressed(FXN_SECURITYINFO_TIMEANDDATE, CHANGE_SUCCESS);
        case BTN_BACK   : return onFxnBackPressed(FXN_SECURITYINFO_TIMEANDDATE);
        default         : return FXN_SECURITYINFO_TIMEANDDATE;
	} 
}


// MENU 3.3
Uint16 
mnuSecurityInfo_AccessTech(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_ACCESSTECH;

	if (isUpdateDisplay) 
	{
		(COIL_UNLOCKED.val) ? updateDisplay(SECURITYINFO_ACCESSTECH,ENGAGELOCK) : updateDisplay(SECURITYINFO_ACCESSTECH,LOCKED);
	}

	switch (input)	
	{
        case BTN_VALUE   : return onNextPressed(MNU_SECURITYINFO_DIAGNOSTICS);
		case BTN_STEP 	:
			isUpdateDisplay = TRUE;
			return (COIL_UNLOCKED.val) ? MNU_SECURITYINFO_ACCESSTECH : FXN_SECURITYINFO_ACCESSTECH;
        case BTN_BACK   : return onNextPressed(MNU_SECURITYINFO);
		case BTN_ENTER 	:
			isUpdateDisplay = TRUE;
			if (COIL_UNLOCKED.val)
			{
				COIL_UNLOCKED.val = FALSE;
				isTechMode = FALSE;
				isTechModeRequested = FALSE;
                Swi_post(Swi_writeNand);
                return onNextMessagePressed(FXN_SECURITYINFO_ACCESSTECH, CHANGE_SUCCESS);
			}
			return MNU_SECURITYINFO_ACCESSTECH;
		default			: return MNU_SECURITYINFO_ACCESSTECH;
	}
}


// FXN 3.3
Uint16 
fxnSecurityInfo_AccessTech(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_ACCESSTECH;

    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_ACCESSTECH, MNU_SECURITYINFO_ACCESSTECH); }

    displayFxn(SECURITYINFO_ACCESSTECH, 0, 0);

	char a[16] = {0};
	memcpy(a,lcdLine1,16);
	int val = atoi(a);

	switch (input)	{
		case BTN_VALUE 	: return onFxnValuePressed(FXN_SECURITYINFO_ACCESSTECH,FALSE, 0);
		case BTN_STEP 	: return onFxnStepPressed(FXN_SECURITYINFO_ACCESSTECH,5); // 0000'\0' - 5 digits
		case BTN_ENTER 	:
			if (val == REG_PASSWORD)
			{
				COIL_UNLOCKED.val = TRUE;
				isTechMode = FALSE;
				isTechModeRequested = FALSE;
                Swi_post(Swi_writeNand);
				return onNextMessagePressed(FXN_SECURITYINFO_ACCESSTECH, GOOD_PASS);
			}
			else if (val == 1343)
			{
				if (!isTechModeRequested)
				{
					isTechModeRequested = TRUE;
					isTechMode = FALSE;
					COIL_UNLOCKED.val = FALSE;
                	Swi_post(Swi_writeNand);
					return onNextMessagePressed(FXN_SECURITYINFO_ACCESSTECH, BAD_PASS);
				}
				else
				{
					isTechModeRequested = FALSE;
					isTechMode = TRUE;
					COIL_UNLOCKED.val = TRUE;
                	Swi_post(Swi_writeNand);
					return onNextMessagePressed(FXN_SECURITYINFO_ACCESSTECH, "Tech Mode Enbld");
				}
			}
			else 
			{
				isTechModeRequested = FALSE;
				isTechMode = FALSE;
				COIL_UNLOCKED.val = FALSE;
               	Swi_post(Swi_writeNand);

				return onNextMessagePressed(FXN_SECURITYINFO_ACCESSTECH, BAD_PASS);
			}
        case BTN_BACK   : return onFxnBackPressed(FXN_SECURITYINFO_ACCESSTECH);
		default			: return FXN_SECURITYINFO_ACCESSTECH;
	}
}


// MENU 3.4
Uint16
mnuSecurityInfo_Diagnostics(const Uint16 input)
{
    if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_DIAGNOSTICS;
    static Uint8 i = 0;
    static Uint8 index = 0;
    static Uint8 errors[MAX_ERRORS];
    static Uint8 errorCount = 0;
    static int DIAGNOSTICS_PREV = -1;

    if (isUpdateDisplay || (DIAGNOSTICS != DIAGNOSTICS_PREV))
    {
        diagnose(&i, &index, &errorCount, errors, &DIAGNOSTICS_PREV);

        if (errorCount > 0)
        {
            index = errors[i];  // Get error index
            sprintf(lcdLine0,"3.4 Diagnos: %*d",3,errorCount);
            sprintf(lcdLine1,"%*s",16,errorType[index]);
            updateDisplay(lcdLine0,lcdLine1);
        }
        else
        {
            errorCount = 0;
            sprintf(lcdLine0,"3.4 Diagnos: %*d",3,errorCount);
            updateDisplay(lcdLine0,BLANK);
        }
    }

    switch (input)
    {
        case BTN_VALUE  : return onNextPressed(MNU_SECURITYINFO_CHANGEPASSWORD);
        case BTN_STEP   :
            isUpdateDisplay = TRUE;
            return (DIAGNOSTICS > 0) ? FXN_SECURITYINFO_DIAGNOSTICS : MNU_SECURITYINFO_DIAGNOSTICS;
        case BTN_BACK   : return onNextPressed(MNU_SECURITYINFO);
        default         : return MNU_SECURITYINFO_DIAGNOSTICS;
    }
}


// FXN 3.4
Uint16 
fxnSecurityInfo_Diagnostics(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_DIAGNOSTICS;
    
	static Uint8 i = 0;						// error index
	static Uint8 index = 0;					// General index
	static Uint8 errorCount = 0;			// Total error count
	static Uint8 errors[MAX_ERRORS];		// Error container
	static int DIAGNOSTICS_PREV = -1;		// Error count update trigger
    if (DIAGNOSTICS != DIAGNOSTICS_PREV) diagnose(&i, &index, &errorCount, errors, &DIAGNOSTICS_PREV);
	sprintf(lcdLine0,"3.4 Diagnos: %*d",3,errorCount);
	displayLcd(lcdLine0,LCD0);
	index = errors[i];	// Get error index
	displayLcd(errorType[index],LCD1);

	switch (input)	
	{
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO_DIAGNOSTICS);
		case BTN_VALUE 	: 
			i++;	
			if (i > errorCount-1) i = 0;
			return FXN_SECURITYINFO_DIAGNOSTICS;
		default			: return FXN_SECURITYINFO_DIAGNOSTICS;
	}
}


// MENU 3.5
Uint16 
mnuSecurityInfo_ChangePassword(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_CHANGEPASSWORD;

   	if (isUpdateDisplay) updateDisplay(SECURITYINFO_CHANGEPASSWORD, BLANK);

	switch (input)
	{
		case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_RESTART);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_CHANGEPASSWORD,MNU_SECURITYINFO_CHANGEPASSWORD,SECURITYINFO_CHANGEPASSWORD);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_CHANGEPASSWORD;
	}
}


// FXN 3.5
Uint16 
fxnSecurityInfo_ChangePassword(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_CHANGEPASSWORD;

    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_CHANGEPASSWORD, MNU_SECURITYINFO_CHANGEPASSWORD); }

    displayFxn(SECURITYINFO_CHANGEPASSWORD, 0000, 0);

    switch (input)  {
        case BTN_VALUE  : return onFxnValuePressed(FXN_SECURITYINFO_CHANGEPASSWORD, FALSE, 0);
        case BTN_STEP   : return onFxnStepPressed(FXN_SECURITYINFO_CHANGEPASSWORD,5); // 0000'\0' - 5 digit
        case BTN_ENTER  : return onFxnEnterPressed(FXN_SECURITYINFO_CHANGEPASSWORD, 9999, 0, NULL_VAR, NULL_DBL, &REG_PASSWORD);
        case BTN_BACK   : return onFxnBackPressed(FXN_SECURITYINFO_CHANGEPASSWORD);
        default         : return FXN_SECURITYINFO_CHANGEPASSWORD;
	}
}


// MENU 3.6
Uint16 
mnuSecurityInfo_Restart(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_RESTART;
   	if (isUpdateDisplay) updateDisplay(SECURITYINFO_RESTART, BLANK);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_FACTRESET);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_RESTART,MNU_SECURITYINFO_RESTART,SECURITYINFO_RESTART);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_RESTART;
	}
}


// FXN 3.6
Uint16 
fxnSecurityInfo_Restart(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_RESTART;

    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_RESTART, MNU_SECURITYINFO_RESTART); }
	static BOOL isEntered = FALSE;

 	if (isUpdateDisplay) updateDisplay(SECURITYINFO_RESTART, ENTER_RESTART);
	(isEntered) ? blinkLcdLine1(STEP_CONFIRM, BLANK) : blinkLcdLine1(ENTER_RESTART, BLANK);

	switch (input)	
	{
		case BTN_STEP 	:
			if (!isEntered) return FXN_SECURITYINFO_RESTART;
			displayLcd("   RESTARTING   ",LCD1);
			for (;;);
		case BTN_BACK 	:
			isEntered = FALSE;
			return onNextPressed(MNU_SECURITYINFO_RESTART);
		case BTN_ENTER 	:
			isEntered = TRUE;
			return FXN_SECURITYINFO_RESTART;
		default			:
			return FXN_SECURITYINFO_RESTART;
	}
}


// MENU 3.7	
Uint16 
mnuSecurityInfo_FactReset(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_FACTRESET;

	if (isUpdateDisplay) updateDisplay(SECURITYINFO_FACTRESET, BLANK);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_PROFILE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_FACTRESET,MNU_SECURITYINFO_FACTRESET,SECURITYINFO_FACTRESET);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_FACTRESET;
	}
}


// FXN 3.7	
Uint16 
fxnSecurityInfo_FactReset(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_FACTRESET;

    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_FACTRESET, MNU_SECURITYINFO_FACTRESET); }

	static BOOL isEntered = FALSE;

	if (isUpdateDisplay) updateDisplay(SECURITYINFO_FACTRESET, ENTER_RESET);

    (isEntered) ? blinkLcdLine1(STEP_CONFIRM, BLANK) : blinkLcdLine1(ENTER_RESET, BLANK);

	switch (input)	
	{
		case BTN_STEP 	:
			if (!isEntered) return FXN_SECURITYINFO_FACTRESET;
            COIL_LOCKED_SOFT_FACTORY_RESET.val = FALSE;   // Unlock SOFT_RESET in reloadFactoryDefault()
            COIL_LOCKED_HARD_FACTORY_RESET.val = TRUE;    // lock HARD_RESET in reloadFactoryDefault()
            Swi_post(Swi_writeNand);
			for (;;);
		case BTN_ENTER 	:
			isEntered = TRUE;
			return FXN_SECURITYINFO_FACTRESET;
		case BTN_BACK 	:
			isEntered = FALSE;
            isUpdateDisplay = TRUE;
			return onFxnBackPressed(FXN_SECURITYINFO_FACTRESET);
		default			:
			return FXN_SECURITYINFO_FACTRESET;
	}
}

// MENU 3.8
Uint16 
mnuSecurityInfo_Profile(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_PROFILE;

	if (isUpdateDisplay) updateDisplay(SECURITYINFO_PROFILE, BLANK);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_UPGRADE);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_PROFILE,MNU_SECURITYINFO_PROFILE,SECURITYINFO_PROFILE);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_PROFILE;
	}
}


// FXN 3.8
Uint16 
fxnSecurityInfo_Profile(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_PROFILE;

    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_PROFILE, MNU_SECURITYINFO_PROFILE); }

	const char delim[] = ".csv";
	char csv_file[MAX_NAME_LENGTH];
	static BOOL isDownload = TRUE;
	static BOOL isSelected = FALSE;
	static int csvIndex = 0;

	if (isUpdateDisplay) 
	{
		isSelected = FALSE;
		isDownload = TRUE;
		csvIndex = 0;
		usbStatus = 0;
		resetCsvStaticVars();

		/* display LCD0 */
		updateDisplay(SECURITYINFO_PROFILE, MOUNTING_USB);
		usb_osalDelayMs(200);

	    /* enumerate usb */
    	Swi_post( Swi_usbhMscDriveOpen );
    	Swi_post( Swi_enumerateUsb );
		(isUsbMounted) ? displayLcd(USB_MOUNTED,LCD1) : displayLcd(USB_UNMOUNTED, LCD1);
		usb_osalDelayMs(1000);
		isUpdateDisplay = FALSE;
	}

	/* usb drive is not deteced */
	if (!isUsbMounted) return onFxnBackPressed(FXN_SECURITYINFO_PROFILE);

	if (isDownload)
	{
        if (isCsvDownloadSuccess) 
        {
			memcpy(lcdLine1,LOAD_SUCCESS,16);
            return notifyMessageAndExit(FXN_SECURITYINFO_PROFILE,MNU_SECURITYINFO_PROFILE);
        }
		else if (isDownloadCsv)
		{
            if ((usbStatus > 1) && (usbStatus < 17)) blinkLcdLine1(USB_CODE[usbStatus], BLANK);
			else blinkLcdLine1(LOADING,BLANK);
		}
        else blinkLcdLine1(DOWNLOAD, BLANK); 
	}
	else
	{
		if (isScanSuccess) 
    	{    
        	int i = 0; 
        	char csv_files[MAX_CSV_ARRAY_LENGTH];
        	csv_files[0] = '\0';
        	csv_file[0] = '\0';
        	strcpy(csv_files, CSV_FILES);
        	char *ptr = strtok(csv_files, delim);
        	while (ptr != NULL)
        	{
            	if (csvIndex == i) sprintf(csv_file, "%s", ptr);
            	ptr = strtok(NULL, delim);
            	i++;
        	}

			if (isSelected) blinkLcdLine1(csv_file,STEP_CONFIRM);
        	else blinkLcdLine1(csv_file,BLANK);
    	}
		else if (isScanCsvFiles || isUploadCsv) blinkLcdLine1(LOADING,BLANK);
		else blinkLcdLine1(UPLOAD, BLANK);
	}

    switch (input)  
    {    
        case BTN_VALUE  :
            if (isScanSuccess) (csvIndex < (csvCounter-1)) ? (csvIndex++) : (csvIndex = 0);
            else (isDownload = !isDownload);
            return FXN_SECURITYINFO_PROFILE;
        case BTN_STEP  :
            if (isScanSuccess && isSelected)
            {
                isScanSuccess = FALSE;
                CSV_FILES[0] = '\0';
                sprintf(CSV_FILES,csv_file);
                isUploadCsv = TRUE;
				Swi_post(Swi_uploadCsv);
            }
            return FXN_SECURITYINFO_PROFILE;
		case BTN_ENTER  :
			if (isScanSuccess) 
			{
				isSelected = TRUE;
				return FXN_SECURITYINFO_PROFILE;
			}

            if (isDownload) 
			{
				isDownloadCsv = TRUE;
				Swi_post(Swi_downloadCsv);
			}
			else
			{
				isScanCsvFiles = TRUE;
				Swi_post(Swi_scanCsvFiles);
			}
            return FXN_SECURITYINFO_PROFILE;
        case BTN_BACK   : return onFxnBackPressed(FXN_SECURITYINFO_PROFILE);
        default         : return FXN_SECURITYINFO_PROFILE;
    } 
}


// MENU 3.9
Uint16 
mnuSecurityInfo_Upgrade(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return MNU_SECURITYINFO_UPGRADE;

  	if (isUpdateDisplay) updateDisplay(SECURITYINFO_UPGRADE, BLANK);

	switch (input)	
	{
        case BTN_VALUE 	: return onNextPressed(MNU_SECURITYINFO_INFO);
		case BTN_STEP 	: return onMnuStepPressed(FXN_SECURITYINFO_UPGRADE,MNU_SECURITYINFO_UPGRADE,SECURITYINFO_UPGRADE);
		case BTN_BACK 	: return onNextPressed(MNU_SECURITYINFO);
		default			: return MNU_SECURITYINFO_UPGRADE;
	}
}


// FXN 3.9
Uint16 
fxnSecurityInfo_Upgrade(const Uint16 input)
{
	if (I2C_TXBUF.n > 0) return FXN_SECURITYINFO_UPGRADE;
    if (isMessage) { return notifyMessageAndExit(FXN_SECURITYINFO_UPGRADE, MNU_SECURITYINFO_UPGRADE); }

	if (isUpdateDisplay) 
	{
		updateDisplay(SECURITYINFO_UPGRADE, MOUNTING_USB);
		usb_osalDelayMs(200);

	    /* enumerate usb */
    	Swi_post( Swi_usbhMscDriveOpen );
    	Swi_post( Swi_enumerateUsb );

		(isUsbMounted) ? displayLcd(USB_MOUNTED,LCD1) : displayLcd(USB_UNMOUNTED, LCD1);
		usb_osalDelayMs(1000);

		isUpdateDisplay = FALSE;
	}

	/* usb drive is not deteced. operation cancelled. */
	if (!isUsbMounted) return onFxnBackPressed(FXN_SECURITYINFO_UPGRADE);

	/* usb successfully mounted. press enter to upgrade */
	blinkLcdLine1(ENTER_UPGRADE, BLANK);

    switch (input)  {
	    case BTN_ENTER   : 
			if (isUsbMounted) Swi_post( Swi_upgradeFirmware );
			return FXN_SECURITYINFO_UPGRADE;
        case BTN_BACK   : return onFxnBackPressed(FXN_SECURITYINFO_UPGRADE);
        default         : return FXN_SECURITYINFO_UPGRADE;
	}
}
