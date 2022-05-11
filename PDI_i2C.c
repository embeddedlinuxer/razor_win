/*------------------------------------------------------------------------
* This Information is proprietary to Phase Dynamics Inc, Richardson, Texas
* and MAY NOT be copied by any method or incorporated into another program
* without the express written consent of Phase Dynamics Inc. This information
* or any portion thereof remains the property of Phase Dynamics Inc.
* The information contained herein is believed to be accurate and Phase
* Dynamics Inc assumes no responsibility or liability for its use in any way
* and conveys no license or title under any patent or copyright and makes
* no representation or warranty that this Information is free from patent
* or copyright infringement.
*
* Copyright (c) 2018 Phase Dynamics Inc. ALL RIGHTS RESERVED.
*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------
* PDI_I2C.c
*-------------------------------------------------------------------------
* These two files contain all the source code for generic functions of I2C 
* communication, as well as many specialized functions for reading/writing 
* to particular I2C peripherals. ADCs are read and controlled via I2C, 
* and are used to read analog input for density, to acquire temperature 
* and reflected power readings from the oscillator board.
* I2C is also used to control the Analog Output value as 
* well as the pin expanders that operate the LCD and the infrared LEDs. 
* Note: very rarely an I2C peripheral will hang, i.e. not release the data 
* line. (That infernal ribbon cable and various connectors are likely to 
* blame.) I2C_Recover() fixes this by sending pulses on SCL until the 
* peripheral lets go of SDA.
* The I2C uses hardware interrupt 6 (I2C_Hwi) to quickly post a SWI and then exit.
*------------------------------------------------------------------------*/

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
///
///     [I2C DAISY CHAIN] 
///
///     TEMP -> VREF -> R_RTC -> DENS -> (W_RTC) -> AO
///
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#undef MENU_H
#include "Globals.h"
#include "PDI_I2C.h"
#include <assert.h>
#include "Menu.h"

///////////////////////////////////////////////////////////////////////////// 
#define R       10                    // 12 KOhm (origianl value)
#define VREF    2.5                   // Reference Voltage
#define BIN16   65536                 // 16 bit DAC multiplier 
#define CMIN    0.04                  // 4 mA min target current
#define CMAX    0.2                   // 20 mA max target current
#define VMIN    (R * CMIN)            // Min voltage
#define VMAX    (R * CMAX)            // Max voltage
#define DMIN    (VMIN * BIN16 / VREF) // Dmin
#define DMAX    (VMAX * BIN16 / VREF) // Dmax
#define ALM_HI  20.5                  // Alarm Hi
#define ALM_LO  3.8                   // Alarm Lo
///////////////////////////////////////////////////////////////////////////// 
#define ADS1112_VREF 2.048
#define MIN_CODE	 -32768
#define PGA		 	 1
#define R_AI		 90.9
#define MAXC		 20
#define MINC		 4
#define VREF		 2.5
///////////////////////////////////////////////////////////////////////////// 

#define I2C_DELAY_TIME  1000
#define KEY             1
#define NO_KEY          0

#define I2C_TEMP    	1
#define I2C_VREF    	2 
#define I2C_RTC     	3 
#define I2C_DENS    	4 
#define I2C_AO      	5 

static BOOL isOk = TRUE;
extern void delayInt(Uint32 count);
static inline void Pulse_ePin(int read, int write, Uint8 lcd_data);
static inline void Pulse_ePin_Manual(int read, int write, Uint8 lcd_data);
static inline Uint8 I2C_Wait_For_TXRDY(void)
{
    Uint32 count = 0;

	while(1)
	{
		if ( CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_ICXRDY) == 1) break;

		count++;
		if (count > 0xFFFF) return 1; // fail
	}

	return 0; // success
}


void
clearI2cSetups()
{
	// clear all error flags resulting from the failed write attempt
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,DISABLE);  //put i2c module in reset
	i2cRegs->ICSTR = CSL_I2C_ICSTR_RESETVAL; //clear interrupt status register
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); //clear interrupt vector register
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,ENABLE);  //take i2c module out of reset
}


//******************************************************************************   
//Function Name     : Bcd2Hex(unsigned char BCDValue)   
//Type              : User defined   
//Return Type       : unsigned char    
//Arguments         : unsigned char BCDValue   
//Details           : convert BCD to Hex    
//Autor             : Nibu   
//******************************************************************************   
   
unsigned char Bcd2Hex(unsigned char BCDValue)   
{   
  unsigned char temp;   
  unsigned char HexVal;   
  temp = BCDValue;   
  HexVal = 0;   
  while(1)   
  {   
    /// Get the tens digit by doing multiple subtraction of 10 from the binary value. 
    if(temp >= 10)   
    {   
      temp -= 10;   
      HexVal += 0x10;   
    }   
    else // Get the ones digit by adding the remainder.   
    {   
      HexVal += temp;   
      break;   
    }   
  }   
  return(HexVal);   
}   
   
//******************************************************************************   
//Function Name     : Hex2Bcd(unsigned char HexVal)   
//Type              : User defined   
//Return Type       : unsigned char    
//Arguments         : unsigned char HexVal   
//Details           : convert hex to Bcd   
//Autor             : Nibu   
//******************************************************************************   
   
// Input range - 00 to 99.   
unsigned char Hex2Bcd(unsigned char HexVal)   
{   
    unsigned char temp;   
    temp = HexVal;   
    // Shifting upper digit right by 1 is same as multiplying by 8.   
    temp >>= 1;   
    // Isolate the bits for the upper digit.   
    temp &= 0x78;   
    return(temp + (temp >> 2) + (HexVal & 0x0f));   
}  

/*********************************************************************************
 * Init_I2C()	- Initialize the I2C Module
 * 				  Master Transmit mode, repeat on, set slave address, etc.
 ********************************************************************************/
void Init_I2C(void)
{
	int i;
	Hwi_disableInterrupt(6);
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,DISABLE);  //put i2c module in reset
	i2cRegs->ICSTR = CSL_I2C_ICSTR_RESETVAL;

	// initialize I2C buffers
	I2C_TXBUF.head = 0;
	I2C_TXBUF.tail = 0;
	I2C_RXBUF.head = 0;
	I2C_RXBUF.tail = 0;
	I2C_TXBUF.n = 0;
	I2C_RXBUF.n = 0;

	for(i=0;i<MAX_BFR_SIZE;i++)
	{
		I2C_TXBUF.buff[i] = 0;
		I2C_RXBUF.buff[i] = 0xFF;
	}

	// convince slave devices to let go
	I2C_Recover(); 

	// i2c mode register
	i2cRegs->ICMDR = CSL_FMKT(I2C_ICMDR_MST,MASTER_MODE) 	// master mode
				   | CSL_FMKT(I2C_ICMDR_TRX,TX_MODE) 		// transmitter mode
				   | CSL_FMKT(I2C_ICMDR_RM,ENABLE)			// repeat mode
			       | CSL_FMKT(I2C_ICMDR_STT,SET); 			// enable start

	i2cRegs->ICCLKL  = CSL_FMK(I2C_ICCLKL_ICCL,0x07); 		// set i2c clock  low time divide down value, 0-FFFFh
	i2cRegs->ICCLKH  = CSL_FMK(I2C_ICCLKH_ICCH,0x08);		// set i2c clock high time divide down value, 0-FFFFh
	i2cRegs->ICPSC   = CSL_FMK(I2C_ICPSC_IPSC,0x0E); 		// i2c prescaler = 14 + 1 --> 150MHz input clock scaled to 10MHz, serial clock = 400kHz
	i2cRegs->ICSAR   = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_XPANDR); 	// set Slave Address to 0x20
	I2C_FINISHED_TX = TRUE;

	// read ICIVR until it's cleared of all flags
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	// bring i2c module out of reset
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,ENABLE);  

	//////////////////////////////////////////////////////////////////////////////////
	///	Each ADC takes: 1000 ticks + 600 for callback fxn =  0.24 seconds
	/// Three ADCs mean that a sample is taken from each ADC every 0.24*3 ~= .72 seconds
	//////////////////////////////////////////////////////////////////////////////////

	Clock_start(I2C_ADC_Read_Temp_Clock);

	Hwi_enableInterrupt(6);
}

// Basically identical to Init_I2C but we leave the TX buffer alone
void Reset_I2C(Uint8 isKey, Uint32 I2C_KEY)
{
    /// if Swi_disabled(), then restore
    if (isKey) Swi_restore(I2C_KEY);

	int i;
	
	Hwi_disableInterrupt(6);

	//////////////////////////////////////////
	// Stop any and all I2C-related clocks
	//////////////////////////////////////////
	/// TEMPERATURE -> VREF -> Read_RTC -> DENSITY -> (Write_RTC) -> AO

	// stop AI Temperature
	Clock_stop(I2C_ADC_Read_Temp_Callback_Clock_Retry);
	Clock_stop(I2C_ADC_Read_Temp_Callback_Clock);
	Clock_stop(I2C_ADC_Read_Temp_Clock_Retry);
	Clock_stop(I2C_ADC_Read_Temp_Clock);

	// stop AI VERF
	Clock_stop(I2C_ADC_Read_VREF_Callback_Clock_Retry);
	Clock_stop(I2C_ADC_Read_VREF_Callback_Clock);
	Clock_stop(I2C_ADC_Read_VREF_Clock_Retry);
	Clock_stop(I2C_ADC_Read_VREF_Clock);

    // stop Read-RTC clocks
	Clock_stop(I2C_DS1340_Read_RTC_Clock_Retry);
	Clock_stop(I2C_DS1340_Read_RTC_Clock);

	// stop AI Density
	Clock_stop(I2C_ADC_Read_Density_Callback_Clock_Retry);
	Clock_stop(I2C_ADC_Read_Density_Callback_Clock);
	Clock_stop(I2C_ADC_Read_Density_Clock_Retry);
	Clock_stop(I2C_ADC_Read_Density_Clock);

    // stop Write-RTC clocks
    Clock_stop(I2C_DS1340_Write_RTC_Clock_Retry);
	Clock_stop(I2C_DS1340_Write_RTC_Clock);

	// stop AO clocks
	Clock_stop(I2C_Update_AO_Clock_Retry);
	Clock_stop(I2C_Update_AO_Clock);

	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,DISABLE);  //put i2c module in reset
	i2cRegs->ICSTR = CSL_I2C_ICSTR_RESETVAL;

	// initialize I2C RX buffer
	I2C_RXBUF.head = 0;
	I2C_RXBUF.tail = 0;
	I2C_RXBUF.n = 0;
	for(i=0;i<MAX_BFR_SIZE;i++) I2C_RXBUF.buff[i] = 0xFF;

	// convince slave devices to let go
	I2C_Recover();	

	// i2c mode register
	i2cRegs->ICMDR = CSL_FMKT(I2C_ICMDR_MST,MASTER_MODE) 		// master mode
							| CSL_FMKT(I2C_ICMDR_TRX,TX_MODE) 	// transmitter mode
							| CSL_FMKT(I2C_ICMDR_RM,ENABLE) 	// repeat mode
							| CSL_FMKT(I2C_ICMDR_STT,SET); 		// enable start

	// set i2c clock  low time divide down value, 0-FFFFh
	i2cRegs->ICCLKL  = CSL_FMK(I2C_ICCLKL_ICCL,0x07);

	// set i2c clock high time divide down value, 0-FFFFh
	i2cRegs->ICCLKH  = CSL_FMK(I2C_ICCLKH_ICCH,0x08);

	// i2c prescaler = 14 + 1 --> 300MHz scaled to 10MHz
	i2cRegs->ICPSC   = CSL_FMK(I2C_ICPSC_IPSC,0x0E);

	// set Slave Address to 0x20
	i2cRegs->ICSAR   = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_XPANDR); 

	I2C_FINISHED_TX = TRUE;

	// read ICIVR until it's cleared of all flags
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	// bring i2c module out of reset
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,ENABLE);  

    //////////////////////////////////////////////////////////////////////////////////
	///	Each ADC takes: 1000 ticks + 600 for callback fxn =  0.24 seconds
	/// Three ADCs mean that a sample is taken from each ADC every 0.24*3 ~= .72 seconds
	//////////////////////////////////////////////////////////////////////////////////

	Clock_start(I2C_ADC_Read_Temp_Clock);

	Hwi_enableInterrupt(6);
}

//	This function changes the data and clock pin to GPIO and issues
//	10 clock pulses to encourage slave devices to release the darn data line
int I2C_Recover(void)
{
	int i, j, swikey, hwikey;
	swikey = Swi_disable();
	hwikey = Hwi_disableInterrupt(6);
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,DISABLE);  //put i2c module in reset
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,ENABLE);  //bring i2c module out of reset

#pragma MUST_ITERATE(9)
	for (i=0;i<9;i++)
	{
#pragma MUST_ITERATE(30)
		for (j=0;j<30;j++); //wait 10 clock cycles (I hope this is right...)
#pragma MUST_ITERATE(30)
		for (j=0;j<30;j++); //wait 10 clock cycles (I hope this is right...)
	}

	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,DISABLE);  		// put i2c module in reset

	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_IRS,ENABLE);  		// bring i2c module out of reset

	Hwi_restoreInterrupt(6, hwikey);
	Swi_restore(swikey);
	return 0;
}

/*********************************************************************************
 * init_LCD()	- Initialize the LCD
 ********************************************************************************/
void Init_LCD(void)
{
	int i;
	char trans_val[I2C_INIT_NUM_CHARS]={LCD_FUNC_SET,LCD_FUNC_SET,LCD_DISP_ON,
			LCD_DISP_CLR,LCD_ENTRY_MODE,LCD_SET_DDRAM_ADDR};

	/// configure gpioPin#135 as an output (LCD)
	ctrlGpioPin(135, GPIO_CTRL_SET_DIR, FALSE, NULL);

	for(i=0; i<(sizeof(trans_val)/sizeof(trans_val[0])); i++)
	{
		Pulse_ePin_Manual(0,0,trans_val[i]);
		delayInt(0xFFFF); // wait for LCD to finish instruction
	}

	LCD_setcursor(0, 0); //turn off cursor
	Clock_start(I2C_LCD_Clock);
	Clock_start(I2C_Start_Pulse_MBVE_Clock);
	Clock_start(Process_Menu_Clock);
	Clock_start(logData_Clock);
}

void Init_MBVE(void)
{
	I2C_BUTTON_CHOOSER = I2C_BUTTON_NONE;
	I2C_BUTTON_STEP = 0;
	I2C_BUTTON_BACK = 0;
	I2C_BUTTON_VALUE = 0;
	I2C_BUTTON_ENTER = 0;

	I2C_START_CLR;
	I2C_STOP_SET;

	I2C_Wait_For_Stop();

	i2cRegs->ICSAR 	= CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_MBVE); 	//Set Slave Address to 0x21
	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICXRDY,ENABLE); 	//unmask the ICRRDY interrupt

	I2C_RM_OFF;
	I2C_STOP_SET;
	I2C_MASTER_MODE;
	I2C_CNT_2BYTE;

	// read ICIVR until it's cleared of all flags
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);
	I2C_START_SET;
	I2C_Wait_For_Start();
	I2C_Wait_To_Send();
	i2cRegs->ICDXR = 0x00;
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	i2cRegs->ICDXR = 0x00;
	I2C_Wait_For_TXRDY();	//poll transmitter ready

	/* set xpandr */
	setLcdXpandr();

	/// enable interrupts on GPIO banks 6 and 8
	CSL_FINST(gpioRegs->BINTEN,GPIO_BINTEN_EN3,ENABLE); //added
	CSL_FINST(gpioRegs->BINTEN,GPIO_BINTEN_EN8,ENABLE);
	CSL_FINST(gpioRegs->BINTEN,GPIO_BINTEN_EN6,ENABLE);

	/// configure gpio edge trigger interrupt
	ctrlGpioPin(65,GPIO_CTRL_SET_FE_INTR, TRUE, NULL); // gpioPin(65) FAL_TRIG
	ctrlGpioPin(65,GPIO_CTRL_SET_RE_INTR, TRUE, NULL); // gpioPin(65) RIS_TRIG
	ctrlGpioPin(61,GPIO_CTRL_SET_FE_INTR, TRUE, NULL); // gpioPin(61) FAL_TRIG
	ctrlGpioPin(61,GPIO_CTRL_SET_RE_INTR, TRUE, NULL); // gpioPin(61) RIS_TRIG
}

/***************************************************************************
 * LCD_printch() - prints single character to LCD at specified position
 *
 * @param c -		character to print
 * @param column -	left to right, 0-16
 * @param line -	0=top line 1=bottom line
 * @return 1=bad column or line value
 ***************************************************************************/
int LCD_printch(char c, int column, int line)
{
	//set address
	if(LCD_setaddr(column,line))
		return ERROR_VAL; //return if column or line value is OOB

	//write character to address
	Pulse_ePin(0,1,c);

	return 0;
}

/***************************************************************************
 * LCD_setaddr() - Set address according to column & row
 * @param column
 * @param line (row)
 * @return 1=bad column or line value
 ***************************************************************************/
int LCD_setaddr(int column, int line)
{
	Uint8 lcd_addr = 0x00;
	//check bounds
	if ((column >= 16) || (column < 0))
		return ERROR_VAL; //columns OOB
	if ((line >= 2) || (line < 0))
		return ERROR_VAL; //rows OOB

	if (line == 1) //if 2nd line
		lcd_addr = 0x40;

	lcd_addr |= (Uint8)column;
	lcd_addr |= 0x80; //MSb is always SET for address write

	//set the DDRAM address
	Pulse_ePin(0,0,lcd_addr);
	return 0;
}

void LCD_setBlinking(int column, int line)
{
	LCD_setaddr(column,line);
	LCD_setcursor(1,1);
}

/***************************************************************************
 * displayLcd()
 * @param c - 		String to print to LCD line. If less than 16
 * 					chars, pad with spaces
 * @param line - 	0=top line 1=bottom line
 ***************************************************************************/
void displayLcd(const char c[], int line)
{
	int i, len;
	char outstr[MAX_LCD_WIDTH];
	Uint8 cursor_status;
	Uint32 key;

	// check bounds
	len = strlen(c);
	if (len > MAX_LCD_WIDTH) len = MAX_LCD_WIDTH;
	if (len < 1) return;

	// clear line
	memset(outstr,0x20,MAX_LCD_WIDTH);
	
	// copy string to <outstr>
	strncpy(outstr, c, len); 

	key = Swi_disable();

	LCD_setaddr(0,line);

	cursor_status = MENU.curStat;
	if (MENU.curStat & LCD_CURS_ON) LCD_setcursor(0,0);
		
	// write line
	for (i=0;i<MAX_LCD_WIDTH;i++) Pulse_ePin(0,1,outstr[i]);

	// put cursor back where it was
	LCD_setaddr(MENU.col,MENU.row);

	// restore cursor status
	if (cursor_status & LCD_CURS_ON) LCD_setcursor(1, (cursor_status & LCD_CURS_BLINK) );

	Swi_restore(key);
}


/***************************************************************************
 * LCD_setcursor()
 * @param curs_on - 	1=show cursor
 * @param curs_blink - 	1=enable cursor blinking (ONLY if curs_on = 1)
 ***************************************************************************/
void LCD_setcursor(int curs_on, int curs_blink)
{
	Uint16 key;
	Uint8 lcd_data = 0x0C; // DB3 and DB2 must be set

	key = Swi_disable();
	if (curs_on)
	{
		MENU.curStat = LCD_CURS_ON;
		if(curs_blink)
		{
			lcd_data |= 0x03; //cursor ON, blink ON
			MENU.curStat |= LCD_CURS_BLINK;
		}
		else
		{
			lcd_data |= 0x02; //cursor ON, blink OFF
			MENU.curStat |= LCD_CURS_NOBLINK;
		}
	}
	else //cursor OFF, blink OFF
		MENU.curStat = LCD_CURS_OFF | LCD_CURS_NOBLINK;

	Pulse_ePin(0,0,lcd_data);
	Pulse_ePin(0,0,lcd_data);
	Swi_restore(key);
}

/*****************************************************************************
 * Pulse_ePin() - Turn E Pin On and Off again
 * TODO: Add error checking to detect possible error in parameters
 * 			e.g. sending instruction command but mistakenly with <write> = 1
 *
 * @param read - read command
 * @param write - write command
 *****************************************************************************/
static inline void Pulse_ePin(int read, int write, Uint8 lcd_data)
{
	Uint8 MSB_eOFF;
	Uint8 MSB_eON;
	Uint16 key;

	if (read == write) // if both are zero (or one) -> instruction command
		MSB_eOFF = 0x00;
	else if (read == 1) //read command
		MSB_eOFF = 0x03;
	else if (write == 1) // write command
		MSB_eOFF = 0x01;
	else
		return; // improper parameter values - must be 0 or 1

	MSB_eON = MSB_eOFF | 0x04; // Add the E pin

	key = Swi_disable();

	// E Pin Off
	I2C_SendByte(lcd_data);
	I2C_SendByte(MSB_eOFF);

	//E Pin On
	I2C_SendByte(lcd_data);
	I2C_SendByte(MSB_eON);

	// E Pin Off again
	I2C_SendByte(lcd_data);
	I2C_SendByte(MSB_eOFF);

	Swi_restore(key);
}

/*****************************************************************************
 * Pulse_ePin_Manual() - Turn E Pin On and Off again manually
 *
 * @param read - read command
 * @param write - write command
 *****************************************************************************/
static inline void Pulse_ePin_Manual(int read, int write, Uint8 lcd_data)
{
	volatile Uint8 MSB_eOFF;
	volatile Uint8 MSB_eON;
	Uint32 key;

	if (read == write) // if both are zero (or one) -> instruction command
		MSB_eOFF = 0x00;
	else if (read == 1) //read command
		MSB_eOFF = 0x03;
	else if (write == 1) // write command
		MSB_eOFF = 0x01;
	else
		return; // improper parameter values - must be 0 or 1

	MSB_eON = MSB_eOFF | 0x04; // Add the E pin

	key = Swi_disable();

	delayInt(0xFFFF);
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	// E Pin Off
	CSL_FINST(i2cRegs->ICSTR,I2C_ICSTR_BB,BUSY);		//set bus busy bit
	i2cRegs->ICDXR = lcd_data;
	delayInt(0xFFFF);
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	i2cRegs->ICDXR = MSB_eOFF;

	// E Pin On
	delayInt(0xFFFF);
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	i2cRegs->ICDXR = lcd_data;
	delayInt(0xFFFF);
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	i2cRegs->ICDXR = MSB_eON;

	// E Pin Off again
	delayInt(0xFFFF);
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	i2cRegs->ICDXR = lcd_data;
	delayInt(0xFFFF);
	I2C_Wait_For_TXRDY();	//poll transmitter ready
	i2cRegs->ICDXR = MSB_eOFF;

	Swi_restore(key);
}


/********************************************************************************
 * I2C_LCD_ClockFxn
 *	Polls I2C_TXBUF to see if there is something to send
 *
 ********************************************************************************/
void I2C_LCD_ClockFxn(void)
{
	if ( (I2C_TXBUF.n > 0) )
	{
		Swi_post(Swi_I2C_TX); //if THR is ready then start sending now
		CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICXRDY,ENABLE); //unmask icxrdy interrupt
	}
}


inline void DisableButtonInts(void)
{
	ctrlGpioPin(61,GPIO_CTRL_CLEAR_FE_INTR, TRUE, NULL); // gpioPin(61) CLR_FAL_TRIG
	ctrlGpioPin(61,GPIO_CTRL_CLEAR_RE_INTR, TRUE, NULL); // gpioPin(61) CLR_RIS_TRIG
	
}

inline void EnableButtonInts(void)
{
	ctrlGpioPin(61,GPIO_CTRL_SET_FE_INTR, TRUE, NULL); // gpioPin(61) SET_FAL_TRIG
	ctrlGpioPin(61,GPIO_CTRL_SET_RE_INTR, TRUE, NULL); // gpioPin(61) SET_FAL_TRIG
}

void I2C_Start_Pulse_MBVE(void)
{
	Uint32	key;
	Uint8	msb, lsb;
	
	if( (I2C_TXBUF.n > 0) || (I2C_FINISHED_TX == FALSE) )
	{
		Clock_start(I2C_Pulse_MBVE_Clock_Short); // try again in ~37.5ms (100 clock ticks)
		return;
	}

	if (I2C_BUTTON_CHOOSER == I2C_BUTTON_NONE)
	{
		Clock_stop(I2C_Start_Pulse_MBVE_Clock);
		key = Swi_disable();
		i2cRegs->ICSAR 	= CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_MBVE); 	//Set Slave Address to 0x21

		// select the STEP button
		I2C_BUTTON_CHOOSER = I2C_BUTTON_S;

		lsb = I2C_BUTTON_S;
		msb = 0x00;

		I2C_START_CLR;
		I2C_STOP_SET;

        /// validate i2c lines
		I2C_Wait_For_Stop();

		i2cRegs->ICSAR 	= CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_MBVE); 	//Set Slave Address to 0x21
		CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,ENABLE); 	//unmask the ICRRDY interrupt

		I2C_RM_OFF;
		I2C_STOP_SET;
		I2C_MASTER_MODE;
		I2C_CNT_2BYTE;

		//read ICIVR until it's cleared of all flags
		while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);
		I2C_START_SET;
		I2C_Wait_For_Start();

		I2C_Wait_To_Send();
		i2cRegs->ICDXR = lsb;

		I2C_Wait_To_Send();
		i2cRegs->ICDXR = msb;

		setLcdXpandr();

		Clock_start(I2C_Pulse_MBVE_Clock); // 100 clock ticks

		Swi_restore(key);

		/* read ICIVR until it's cleared of all flags */
		while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

		/* send start condition to LCD expander */
		I2C_START_SET; 
	}
}

/**********************************************************************************
 * I2C_Pulse_MBVE(): Called by I2C_Pulse_MBVE_Clock and I2C_Start_Pulse_MBVE_Clock
 * - Drives the input to one of the four MBVE buttons
 * - Resends the last two bytes sent to the expander, but with a different "MBVE enable"
 * - Essentially "calls itself" until all four buttons are pulsed
 **********************************************************************************/
void I2C_Pulse_MBVE(void)
{
	Uint32	key;
	Uint8	i2c_lsb, i2c_msb;
    Uint8   button_state_changed = FALSE;
	Uint32	button_pin = 0;
    int     timeout = TRUE;

	if (I2C_BUTTON_CHOOSER == I2C_BUTTON_NONE)
	{	// we are done with this series of pulses, start the "long" MBVE clock
        // calls I2C_Start_Pulse_MBVE() after "long" delay (800 us)
		Clock_start(I2C_Start_Pulse_MBVE_Clock); 
		return;
	}

	key = Swi_disable();

	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_MBVE); 	//Set Slave Address to 0x21

	/// read gpioPin(96) and check if button pressed
	ctrlGpioPin(96, GPIO_CTRL_READ_INPUT, NULL, &button_pin);

	// if a button is pressed
	if ( button_pin != 0 ) //GPIO2[14] = SSPI_SCS[0] = CP[14] = bank0pin14
	{
		if (I2C_BUTTON_CHOOSER == I2C_BUTTON_S) // STEP
		{
			if (I2C_BUTTON_STEP != TRUE)  
			{
				I2C_BUTTON_STEP = TRUE;
				button_state_changed = TRUE;
			}
		}
		else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_B) // BACK
		{
			if(I2C_BUTTON_BACK != TRUE)
			{
				I2C_BUTTON_BACK = TRUE;
				button_state_changed = TRUE;
			}
		}
		else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_V) // VALUE
		{
			if(I2C_BUTTON_VALUE != TRUE)
			{
				I2C_BUTTON_VALUE = TRUE;
				button_state_changed = TRUE;
			}
		}
		else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_E) // ENTER
		{
			if(I2C_BUTTON_ENTER != TRUE)
			{
				I2C_BUTTON_ENTER = TRUE;
				button_state_changed = TRUE;
			}
		}
	}
	else //not pressed
	{
		if (I2C_BUTTON_CHOOSER == I2C_BUTTON_S)
		{
			if (I2C_BUTTON_STEP != FALSE)
			{
				I2C_BUTTON_STEP = FALSE;
				button_state_changed = TRUE;
			}
		}
		else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_B)
		{
			if(I2C_BUTTON_BACK != FALSE)
			{
				I2C_BUTTON_BACK = FALSE;
				button_state_changed = TRUE;
			}
		}
		else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_V)
		{
			if(I2C_BUTTON_VALUE != FALSE)
			{
				I2C_BUTTON_VALUE = FALSE;
				button_state_changed = TRUE;
			}
		}
		else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_E)
		{
			if(I2C_BUTTON_ENTER != FALSE)
			{
				I2C_BUTTON_ENTER = FALSE;
				button_state_changed = TRUE;
			}
		}
	}

    // reset input register
	i2c_lsb = 0x00;
	i2c_msb = 0x00;

	/// Decide which button to drive next
	if (I2C_BUTTON_CHOOSER == I2C_BUTTON_S)
	{
		i2c_lsb |= I2C_BUTTON_B;	//drive the BACK button next
		I2C_BUTTON_CHOOSER = I2C_BUTTON_B;
	}
	else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_B)
	{
		i2c_lsb |= I2C_BUTTON_V;	//drive the VALUE button next
		I2C_BUTTON_CHOOSER = I2C_BUTTON_V;
	}
	else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_V)
	{	
		i2c_lsb |= I2C_BUTTON_E;	//drive the ENTER button next
		I2C_BUTTON_CHOOSER = I2C_BUTTON_E;
	}
	else if (I2C_BUTTON_CHOOSER == I2C_BUTTON_E)
	{
		i2c_lsb |= I2C_BUTTON_NONE;	//drive none of the buttons next
		I2C_BUTTON_CHOOSER = I2C_BUTTON_NONE;
	}

	I2C_Wait_To_Send(); //finsh sending anything in the THR

	I2C_START_CLR;
	I2C_STOP_SET;
	I2C_Wait_For_Stop();
	i2cRegs->ICSAR 	= CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_MBVE); 	//Set Slave Address to 0x21
	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,ENABLE); 	//unmask the ICRRDY interrupt
	I2C_RM_OFF;
	I2C_STOP_SET;
	I2C_MASTER_MODE;
	I2C_CNT_2BYTE;

	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); //read ICIVR until it's cleared of all flags
	I2C_START_SET;
	I2C_Wait_For_Start();
    timeout = I2C_Wait_To_Send();

    if (timeout) Reset_I2C(KEY, key);
	else	
	{	
		i2cRegs->ICDXR = i2c_lsb;
		if (!I2C_Wait_For_Ack()) 
		{
			if (!I2C_Wait_To_Send())
			{
        		i2cRegs->ICDXR = i2c_msb;
				if (!I2C_Wait_For_Ack())
				{
					setLcdXpandr();
				}
			}
		}	
	}
	
	/* start the "short" MBVE clock */
	Clock_start(I2C_Pulse_MBVE_Clock); // pulse the next button in ~37.5ms 100 clock ticks

	Swi_restore(key);

	/* read ICIVR until it's cleared of all flags */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
	I2C_START_SET;
}


/***************************************************************************
 * I2C_HWI_ISR()
 *
 * HWI Handle: I2C_Hwi
 ***************************************************************************/
void I2C_HWI_ISR(void)
{
	Uint8 intcode;

	intcode = CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE);

	//if ICXRDY caused the interrupt and the I2C THR is still ready
	if( (intcode == CSL_I2C_ICIVR_INTCODE_ICXRDY) && (CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_ICXRDY)))
	{
		if (I2C_TXBUF.n > 0) // if there is something to send
		{
			Swi_post(Swi_I2C_TX); //keep sending from the TX buffer until it's empty
			Hwi_disableInterrupt(6);
		}

		//read ICIVR until it's cleared of all flags
		while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);
	}
}


/***************************************************************************
 * I2C_RX_Fxn()
 *
 * SWI Handle: Swi_I2C_RX
 ***************************************************************************/
void I2C_RX_Fxn(void)
{
	Uint8 i2c_byte;
	unsigned int key;

	key = Swi_disable();

	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_TRX,RX_MODE); //put I2C in RX mode

	i2c_byte = i2cRegs->ICDRR; //read byte
	BfrPut(&I2C_RXBUF, i2c_byte);

	Swi_restore(key);
}


/***************************************************************************
 * I2C_TX_Fxn()
 *
 * SWI Handle: Swi_I2C_TX
 ***************************************************************************/
void I2C_TX_Fxn(void)
{
	Uint8 i2c_byte = 0;
	unsigned int key;

	if (I2C_TXBUF.n <= 0)
	{
		CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICXRDY,DISABLE); //mask icxrdy interrupt
		I2C_TXBUF.n = 0;

		I2C_TXBUF.tail = I2C_TXBUF.head;
		Clock_start(I2C_LCD_Clock);
		I2C_FINISHED_TX = TRUE;
		Hwi_enableInterrupt(6); //re-enable I2C hwi
		return;
	}

	key = Swi_disable();
	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICXRDY,DISABLE); //mask icxrdy interrupt
	I2C_FINISHED_TX = FALSE;
	while(I2C_TXBUF.n > 0)
	{ // data is available from the buffer
		if (!I2C_Wait_To_Send_ARDY())
		{
			i2c_byte = BfrGet(&I2C_TXBUF);
			i2cRegs->ICDXR = i2c_byte;
			CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICXRDY,ENABLE); //unmask the ICXRDY interrupt
		}
		else
		{
			I2C_TXBUF.head = 0;
			I2C_TXBUF.tail = 0;
			I2C_TXBUF.n = 0;
		}
	}
//	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICXRDY,ENABLE); //unmask the ICXRDY interrupt
	I2C_FINISHED_TX = TRUE;
	if (I2C_TXBUF.n < 0)
	{
		I2C_TXBUF.n = 0;
		I2C_TXBUF.tail = I2C_TXBUF.head;
	}
	Clock_start(I2C_LCD_Clock);
	Swi_restore(key);
	Hwi_enableInterrupt(6); //re-enable I2C hwi
}


/*****************************************************************************
 * I2C_SendByte()	- Adds a byte to transmit buffer
 *
 * @param out_byte	- Byte to add to the buffer
 *****************************************************************************/
void I2C_SendByte(Uint8 out_byte)
{
	if (I2C_FINISHED_TX == TRUE) I2C_FINISHED_TX = FALSE;
	if (I2C_TXBUF.n < MAX_BFR_SIZE-1) BfrPut(&I2C_TXBUF,out_byte);
}


static inline int I2C_Wait_To_Send(void)
{
	int count = 0;
	Uint32 key;
	int timeout = FALSE;
	int slave_addr;

	slave_addr = CSL_FEXT(i2cRegs->ICSAR,I2C_ICSAR_SADDR);

	while (CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_ICXRDY) != 1) //wait to send first byte
	{
		if (count > I2C_DELAY_TIME)
		{	//buffer is hung? re-initialize I2C module
			if (slave_addr == I2C_SLAVE_ADDR_XPANDR)
			{
				key = Swi_disable();
				Reset_I2C(KEY, key);
			}

			count = 0;
			timeout = TRUE;
			break;
		}
		count++;
	}
	count++;

	return timeout;
}

//Just like Wait_To_Send but also simultaneously checks ARDY (Register-access-ready interrupt flag bit)
//Only use this with repeat mode!
static inline int I2C_Wait_To_Send_ARDY(void)
{
	Uint32 count = 0;
	Uint32 key;
	int timeout = FALSE;
	int slave_addr;

	slave_addr = CSL_FEXT(i2cRegs->ICSAR,I2C_ICSAR_SADDR);
	while ((CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_ICXRDY) != 1) && (CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_ARDY) != 1))//wait to send first byte
	{
		if (count > I2C_DELAY_TIME)
		{	
			if (slave_addr == I2C_SLAVE_ADDR_XPANDR)
			{
				key = Swi_disable();
				Swi_restore(key);
			}

			count = 0;
			timeout = TRUE;
			break;
		}
		count++;
	}
	count++;

	return timeout;
}
static inline int I2C_Wait_To_Receive(void)
{
    int count = 0; 

    while (CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_ICRRDY) != 1)
    {    
        count++;
        if (count > I2C_DELAY_TIME) return 1;
    }    
    return 0; // success
}


static inline int I2C_Wait_For_Start(void)
{
	Uint32 count = 0;

	while (CSL_FEXT(i2cRegs->ICMDR,I2C_ICMDR_STT))
	{
		count++;
		if (count > I2C_DELAY_TIME) return 1;
	}

	return 0; // SUCCESS
}

static inline int I2C_Wait_For_Stop(void)
{
	Uint32 count = 0;

	while (CSL_FEXT(i2cRegs->ICMDR,I2C_ICMDR_STP))
	{
		count++;
		if (count > I2C_DELAY_TIME)
		{
			I2C_MASTER_MODE; // I2C module switches to slave mode after every STOP condition
			return 1;
		}
	}

	I2C_MASTER_MODE; // I2C module switches to slave mode after every STOP condition
	return 0; // SUCCESS
}

static inline int I2C_Wait_For_Ack(void)
{
	Uint32 count = 0;

	while (CSL_FEXT(i2cRegs->ICSTR,I2C_ICSTR_NACK) != 0)
	{
		count++;
		if (count > 0xFFFF) return 1;
	}

	return 0; // SUCCESS
}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
///
///     [I2C ROUND ROBIN ORDER] 
///
///     TEMP -> VREF -> R_RTC -> DENS -> (W_RTC) -> AO
///
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void I2C_ADC_Read_Temp(void)
{
	if (I2C_TXBUF.n > 0)
	{
		Clock_start(I2C_ADC_Read_Temp_Clock_Retry);
		return;
	}

	Uint32 key;

	key = Swi_disable();

	setStop();

	/// set slave address 0x48
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_ADC);

	/// set transmission (write) mode
	setTx();
	I2C_CNT_1BYTE;
	I2C_Wait_To_Send();

	/* clear all flags and interrupts */
	while (CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); 

	setStart();

	/* set config register */
	i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, 0xFC);

	setStop();

	setLcdXpandr();

	Clock_start(I2C_ADC_Read_Temp_Callback_Clock);

	Swi_restore(key);

	/* clear all flags and interrupts */
	while (CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
	I2C_START_SET; 
}

// This function is called after waiting for the ADC conversion. Reads in value.
void I2C_ADC_Read_Temp_Callback(void)
{
	ctrlGpioPin(TEST_LED1,GPIO_CTRL_SET_OUT_DATA, TRUE, NULL); // LED 1 on DKOH
	ctrlGpioPin(TEST_LED2,GPIO_CTRL_SET_OUT_DATA, FALSE, NULL); // LED 2 off DKOH

	if (I2C_TXBUF.n > 0)
	{
		Clock_start(I2C_ADC_Read_Temp_Callback_Clock_Retry);
		return;
	}

	Uint32 key;
	Uint16 temp_val;
	double temp_dbl;
	static double temp_prev;
	static Uint8 tryAgain = 0;
	Uint8 adc_config;
	Uint8 isPass = 0;

	key = Swi_disable();

	setStop();

	/// set slave address to 0x48
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_ADC); 

	/* set receive (read) mode */
	setRx();
	I2C_CNT_3BYTE;
	I2C_Wait_To_Send();

	/* clear flags and interrupts */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); 

    /* start */
	setStart();
	if (!I2C_Wait_To_Receive())
	{
       	/* read msb */
		temp_val = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D) << 8; 
		if (!I2C_Wait_To_Receive())
		{
       		/* read lsb */
			temp_val |= CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
			if (!I2C_Wait_To_Receive())
			{
       			/* read config */
				adc_config = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
				isPass = 1;
			}
		}
	}

	setStop();

	if (isPass)
	{	
		temp_dbl = (double)temp_val * 2.048/32768.0; 		// convert from ADC code to voltage
		temp_dbl = temp_dbl * 2.5;							// account for voltage divider (2.5x)
		temp_dbl = temp_dbl * 1000.0/12 - 273.15;			// work backward from voltage to current (12.0 kOhm) to K to C

		/* i2c noise filtering */
		(temp_prev != temp_dbl) ? (tryAgain++) : (tryAgain = 0);
		if ((tryAgain > 5) || (temp_prev == 0))
		{
			VAR_Update(&REG_TEMPERATURE,temp_dbl,0);
			temp_prev = temp_dbl;
			tryAgain = 0;
		}
	}
	else clearI2cSetups();

	setLcdXpandr();

	Clock_start(I2C_ADC_Read_VREF_Clock);

	Swi_restore(key);

	/* clear all flags and interrupts */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
	I2C_START_SET; 
}

void I2C_ADC_Read_VREF(void)
{
	if (I2C_TXBUF.n > 0)
	{
		Clock_start(I2C_ADC_Read_VREF_Clock_Retry);
		return;
	}

	Uint32 key;

	key = Swi_disable();

	/// stop
	setStop();

	/// set slave address to 0x48
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_ADC); 

	/// set trnasmission (write) mode
	setTx();
	I2C_CNT_1BYTE;
	I2C_Wait_To_Send();

	/* clear all flags and interrupts */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	setStart();

	/// set config register
	i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, 0xDC);

	setStop();

	setLcdXpandr();

	Clock_start(I2C_ADC_Read_VREF_Callback_Clock);

	Swi_restore(key);

	/* clear all flags and interrupts */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
	I2C_START_SET; 
}

void I2C_ADC_Read_VREF_Callback(void)
{
    if (I2C_TXBUF.n > 0) 
    {
        Clock_start(I2C_ADC_Read_VREF_Callback_Clock_Retry);
        return;
    }

    Uint32 key;
    Uint16 vref_val;
    double vref_dbl;
    Uint8 adc_config;
	Uint8 isPass = 0;

    key = Swi_disable();
	
	/// stop
	setStop();

	/// set slave address to 0x48
    i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_ADC);

	/// set receive (read) mode
	setRx();
    I2C_CNT_3BYTE;
	I2C_Wait_To_Send();

    /// clear flags and interrupts
    while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	setStart();
        
	if (!I2C_Wait_To_Receive())
	{
       	/// read msb
       	vref_val = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D) << 8;
		if (!I2C_Wait_To_Receive())
		{
       		/// read lsb
       		vref_val |= CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
			if (!I2C_Wait_To_Receive())
			{
       			/// read config 
       			adc_config = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
				isPass = 1;
			}
		}
	}

	setStop();

	if (isPass)
	{
    	vref_dbl = (double)vref_val * 2.048/32768.0;// convert from ADC code to voltage
    	vref_dbl = vref_dbl * 2.5; 					// account for voltage divider 2.5
    	REG_OIL_RP = vref_dbl + (REG_OIL_T1.calc_val * REG_TEMP_USER.calc_val) + REG_OIL_T0.calc_val;
	}
	else clearI2cSetups();

	setLcdXpandr();

	Clock_start(I2C_DS1340_Read_RTC_Clock);

    Swi_restore(key);

	/* clear all flags and interrupts */
    while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
    I2C_START_SET; 
}

void I2C_DS1340_Read_RTC(void)
{
	if (I2C_TXBUF.n > 0)
	{
		Clock_start(I2C_DS1340_Read_RTC_Clock_Retry);
		return;
	}

	Uint32 key;
    static int tmp_sec, tmp_min, tmp_hr, tmp_day, tmp_mon, tmp_yr;

	key = Swi_disable();

    if (isOk)
    {
        isOk = FALSE;

        tmp_sec = Hex2Bcd(I2C_DS1340_Read(0x00)&0x7F);   
     	if ((tmp_sec != REG_RTC_SEC) && (tmp_sec > -1) && (tmp_sec < 60)) REG_RTC_SEC = tmp_sec;

       	tmp_min = Hex2Bcd(I2C_DS1340_Read(0x01)&0x7F);
       	if ((tmp_min != REG_RTC_MIN) && (tmp_min > -1) && (tmp_min < 60)) REG_RTC_MIN = tmp_min;

       	tmp_hr  = Hex2Bcd(I2C_DS1340_Read(0x02)&0x3F);   
       	if ((tmp_hr != REG_RTC_HR) && (tmp_hr > -1) && (tmp_hr < 24)) REG_RTC_HR = tmp_hr;

       	tmp_day = Hex2Bcd(I2C_DS1340_Read(0x04)&0x3F);   
       	if ((tmp_day != REG_RTC_DAY) && (tmp_day > 0) && (tmp_day < 32)) REG_RTC_DAY = tmp_day;

       	tmp_mon = Hex2Bcd(I2C_DS1340_Read(0x05)&0x1F);   
       	if ((tmp_mon != REG_RTC_MON) && (tmp_mon > 0) && (tmp_mon < 13)) REG_RTC_MON = tmp_mon;

       	tmp_yr  = Hex2Bcd(I2C_DS1340_Read(0x06)&0xFF);
       	if ((tmp_yr != REG_RTC_YR) && (tmp_yr > -1) && (tmp_yr < 100)) REG_RTC_YR = tmp_yr;

        isOk = TRUE;
    }

	setLcdXpandr();

	Clock_start(I2C_ADC_Read_Density_Clock);

   	Swi_restore(key);

	/* clear all flags and interrupts */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition for lcd extend */
	I2C_START_SET; 
}


int I2C_DS1340_Read(int TIME_ADDR)
{
    int read_val;
	Uint8 isPass = 0;

	setStop();

	/// set Slave Address
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_DS1340); 

	/// set transmission (write) mode
	setTx();
    I2C_CNT_1BYTE;
	I2C_Wait_To_Send();

    /// clear all flags and interrupts
	while (CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/// start
	setStart();

    /// set config register
	i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D,TIME_ADDR);

    /// stop
	setStop();

	/// set receive (read) modes
	setRx();
	I2C_CNT_1BYTE;
	I2C_Wait_To_Send();

	/// clear all flags and interrupts
	while (CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); 

	setStart();

	if (!I2C_Wait_To_Receive()) 
	{
    	/// read msb
		read_val = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D) << 8;
		if (!I2C_Wait_For_Ack()) 
		{
    		/// read lsb
			read_val |= CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
			if (!I2C_Wait_For_Ack())
			{
				isPass = 1;
			}
		}
	}

	setStop();

	if (!isPass) clearI2cSetups();

    return read_val;
}


void I2C_ADC_Read_Density(void)
{
	if(I2C_TXBUF.n > 0)
	{
		Clock_start(I2C_ADC_Read_Density_Clock_Retry);
		return;
	}

	Uint32 key;

	key = Swi_disable();

	/// stop
	setStop();

	/// set slave address to 0x4A
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_ADC2); 

	/// set transmission (write) mode
	setTx();
	I2C_CNT_1BYTE;
	I2C_Wait_To_Send();

	// read ICIVR until it's cleared of all flags
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	setStart();

	/// set config register
	i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, 0xDC);

	setStop();

	setLcdXpandr();

	Clock_start(I2C_ADC_Read_Density_Callback_Clock);

	Swi_restore(key);

	/* read ICIVR until it's cleared of all flags */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
	I2C_START_SET; 
}


void I2C_ADC_Read_Density_Callback(void)
{
    if (I2C_TXBUF.n > 0) 
    {
        Clock_start(I2C_ADC_Read_Density_Callback_Clock_Retry);
        return;
    }

    Uint32 key;
    Uint16 vref_val;
    double vref_dbl;
    Uint8 adc_config;
	Uint8 isPass = 0;

    key = Swi_disable();

	/// stop
	setStop();

	/// set slave address to 0x4A
    i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_ADC2);

	/// set receive (read) mode
	setRx();
    I2C_CNT_3BYTE;
	I2C_Wait_To_Send();

    /// read ICIVR until it's cleared of all flags
    while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	setStart();

	if (!I2C_Wait_To_Receive())
	{
		/// read ms
    	vref_val = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D) << 8;
		if (!I2C_Wait_To_Receive())
		{
			/// read lsb
    		vref_val |= CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
			if (!I2C_Wait_To_Receive())
			{
				/// read config
    			adc_config = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);
				isPass = 1;
			}
		}
	}

	setStop();

	if (isPass)
	{
		// is analog input mode?
		if (REG_OIL_DENS_CORR_MODE == 1)  		
		{
			vref_dbl = ((double)vref_val*ADS1112_VREF)/(-1*MIN_CODE*PGA); 	// convert from ADC code to voltage
   	 		vref_dbl = vref_dbl*VREF; 							 		 	// account for voltage divider
			REG_AI_MEASURE = vref_dbl*(1000.0/R_AI);			 			// work backward from voltage to current (R = 90.9 Kohm)

			double m = (MAXC-MINC)/(REG_AI_TRIMHI-REG_AI_TRIMLO);
			double current = m * (REG_AI_MEASURE-REG_AI_TRIMLO) + MINC;
	
			// update AI trimmed value ///////////////
			REG_AI_TRIMMED = current;

			// update density ////////////////////////
   			double md = (REG_OIL_DENSITY_AI_URV.calc_val-REG_OIL_DENSITY_AI_LRV.calc_val)/(REG_AI_TRIMHI-REG_AI_TRIMLO);
			double density = md * (REG_AI_MEASURE-REG_AI_TRIMLO) + REG_OIL_DENSITY_AI_LRV.calc_val;
   			REG_OIL_DENSITY_AI = density;
		}
	}
	else clearI2cSetups();

	setLcdXpandr();

	if (isWriteRTC) Clock_start(I2C_DS1340_Write_RTC_Clock);
	else Clock_start(I2C_Update_AO_Clock);

    Swi_restore(key);

    /* read ICIVR until it's cleared of all flags */
    while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
    I2C_START_SET; 
}


void I2C_DS1340_Write_RTC(void)
{
    isWriteRTC = FALSE;
    Uint32 key;

	key = Swi_disable();

    /// SET RTC TIME (HH:MN MM/DD/YYYY)
    I2C_DS1340_Write(0x05,REG_RTC_YR_IN);
    I2C_DS1340_Write(0x04,REG_RTC_MON_IN);
    I2C_DS1340_Write(0x01,REG_RTC_HR_IN);
    I2C_DS1340_Write(0x03,REG_RTC_DAY_IN);
    I2C_DS1340_Write(0x00,REG_RTC_MIN_IN);

	setLcdXpandr();

	Clock_start(I2C_Update_AO_Clock);

	Swi_restore(key);

	/* read ICIVR until it's cleared of all flags */
	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

	/* send start condition to LCD expander */
	I2C_START_SET; 
}


void I2C_DS1340_Write(int RTC_ADDR, int RTC_DATA)
{
    if (I2C_TXBUF.n > 0) 
    {
        Clock_start(I2C_DS1340_Write_RTC_Clock_Retry);
        return;
    }

	Uint8 isPass = 0;

	/// stop
	setStop();

	/// set Slave Address
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_DS1340); 

	/// set transmission (write) mode
	setTx();
	I2C_CNT_1BYTE;
	I2C_Wait_To_Send();

	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); //read ICIVR until it's cleared of all flags
    setStart();

    //Note: Nested if-statement used so that we abort the write process at the first sign of failure
    if (!I2C_Wait_For_Start())
    {
		i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D,RTC_ADDR);

        if (!I2C_Wait_For_Ack())
        {
            if (!I2C_Wait_To_Send())
            {
				i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, ((Bcd2Hex((unsigned char)RTC_DATA) >> 8) & 0xFF) );

                if (!I2C_Wait_For_Ack())
                {
                    if (!I2C_Wait_To_Send())
					{
						i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, (Bcd2Hex((unsigned char)RTC_DATA) & 0xFF));
						isPass = 1;
					}
                }
            }
        }
    }

	setStop(); 

	if (!isPass) clearI2cSetups();
}


void I2C_Update_AO(void)
{
	ctrlGpioPin(TEST_LED1,GPIO_CTRL_SET_OUT_DATA, FALSE, NULL); // LED 1 on DKOH
	ctrlGpioPin(TEST_LED2,GPIO_CTRL_SET_OUT_DATA, TRUE, NULL); // LED 2 off DKOH

	if(I2C_TXBUF.n > 0)
    {
        Clock_start(I2C_Update_AO_Clock_Retry);
        return;
    }

    long double percent_val;
    long double dmax;
    long double dmin;
    Uint32 out_data;
    Uint32 key;
    Uint16 in_val;
    Uint16 ctrl_byte;
    Uint8 isPass = 0;

    I2C_START_CLR;
    I2C_STOP_SET;
    I2C_Wait_For_Stop();
    key = Swi_disable();

    // is trimming mode?
    if (COIL_AO_TRIM_MODE.val)
    {
        dmax = DMAX;
        dmin = DMIN;
    }
    else
    {
        dmax = 16*(DMAX-DMIN)/(REG_AO_TRIMHI-4.0) + DMIN;
        dmin = (4-REG_AO_TRIMLO)*(DMAX-DMIN)/(20.0-REG_AO_TRIMLO) + DMIN;
    }

	// is active error && alarm notificaion enabled?
    if ((DIAGNOSTICS > 0) && (REG_AO_ALARM_MODE > 0))
    {
        if (REG_AO_ALARM_MODE == 1) percent_val = ((REG_AO_URV.calc_val-REG_AO_LRV.calc_val)*(ALM_HI-4))/(16*100);
        else if (REG_AO_ALARM_MODE == 2) percent_val = ((REG_AO_URV.calc_val-REG_AO_LRV.calc_val)*(ALM_LO-4))/(16*100);
    }
    else
    {
		/* manual mode */
		if (REG_AO_MODE == 2) 
		{
			percent_val = (REG_AO_MANUAL_VAL-4)/16;
		}

		/* automatic mode */
       	else 
		{
			/* legal boundary */
			percent_val = (REG_WATERCUT.calc_val-REG_AO_LRV.calc_val)/(REG_AO_URV.calc_val-REG_AO_LRV.calc_val);

			/* illegal boundary */	
			if (REG_WATERCUT.calc_val >= REG_AO_URV.calc_val) percent_val = 1.0; 
       		else if (REG_WATERCUT.calc_val <= REG_AO_LRV.calc_val) percent_val = 0;
		}
	}

    /* reverse mode */
    if (REG_AO_MODE == 1) out_data = ((dmax-dmin)*(1.0-percent_val)) + dmin;

	/* normal mode */
    else out_data = ((dmax-dmin)*percent_val) + dmin;

	/* menu 2.4 lcd screen */
	REG_AO_OUTPUT = 16*percent_val + 4;

    /// set Slave Address ////////////////////////////////////////
    i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_DAC);
    //////////////////////////////////////////////////////////////  

    // write to DAC
    I2C_MASTER_MODE;
    CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,DISABLE); //mask the ICRRDY interrupt
    I2C_START_CLR;
    I2C_RM_OFF;
    I2C_STOP_SET;
    I2C_CNT_3BYTE;

    if (I2C_Wait_For_Start())
    {   //timeout - probably a slave device holding the SDA line
        Reset_I2C(KEY, key);
        return;
    }

	while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); //read ICIVR until it's cleared of all flags
    I2C_START_SET;  // initiate sequence

    //Note: Nested if-statement used so that we abort the write process at the first sign of failure
    if (!I2C_Wait_For_Start())
    {
        i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D,I2C_CTRL_BYTE_WL); // control byte: write&load operation

        if (!I2C_Wait_For_Ack())
        {
            if (!I2C_Wait_To_Send())
            {
                i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, ((out_data >> 8) & 0xFF) ); //MSB

                if (!I2C_Wait_For_Ack())
                {
                    I2C_Wait_To_Send();
                    i2cRegs->ICDXR = CSL_FMK(I2C_ICDXR_D, (out_data & 0xFF)); //LSB
					isPass = 1;
                }
            }
        }
    }

	if (isPass)
	{
        I2C_START_CLR;
        I2C_STOP_SET;
        I2C_Wait_For_Stop();

        ///////////// Read back the DAC value and control byte (disabled)//////////////
        I2C_RX_MODE;    //put I2C in RX mode
        I2C_CNT_3BYTE;
        CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,ENABLE); // ICRRDY interrupt
        while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE); //read ICIVR until it's cleared of all flags
        I2C_STOP_SET;
        I2C_START_SET;
        I2C_Wait_For_Start();
        I2C_Wait_To_Receive();
        in_val = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D) << 8; //MSB
        I2C_Wait_To_Receive();
        in_val |= CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);     //LSB
        I2C_Wait_To_Receive();
        ctrl_byte = CSL_FEXT(i2cRegs->ICDRR,I2C_ICDRR_D);   //config
        I2C_Wait_For_Stop();
        ////////////////////////////////////////////////////////////////////
    }
	else clearI2cSetups();

	setLcdXpandr();

    Clock_start(I2C_ADC_Read_Temp_Clock);

    Swi_restore(key);
   
    /* read ICIVR until it's cleared of all flags */
    while(CSL_FEXT(i2cRegs->ICIVR, I2C_ICIVR_INTCODE) != CSL_I2C_ICIVR_INTCODE_NONE);

    /* send start condition to LCD expander */
    I2C_START_SET;
}


void
setLcdXpandr(void)
{
   	I2C_TX_MODE; 										
	I2C_MASTER_MODE;
	I2C_RM_ON;

	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,DISABLE); 				// mask the ICRRDY interrupt
	i2cRegs->ICSAR = CSL_FMK(I2C_ICSAR_SADDR,I2C_SLAVE_ADDR_XPANDR); 	// set slave address to 0x20
	CSL_FINST(i2cRegs->ICMDR,I2C_ICMDR_STP,CLEAR); 						// clear stop bit;
}


void
setStop(void)
{
	I2C_START_CLR;
	I2C_STOP_SET;
	I2C_Wait_For_Stop();
}

void
setStart(void)
{
	I2C_STOP_SET; 
	I2C_START_SET;
	I2C_Wait_For_Start();
}

void
setTx(void)
{
	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,DISABLE); // mask the ICRRDY interrupt
   	I2C_TX_MODE; // I2C in TX MODE
	I2C_RM_ON;
	I2C_MASTER_MODE;
}

void
setRx(void)
{
   	CSL_FINST(i2cRegs->ICIMR,I2C_ICIMR_ICRRDY,ENABLE); // unmask the ICRRDY interrupt
	I2C_RX_MODE; // I2C in RX mode 
	I2C_RM_OFF;
	I2C_MASTER_MODE;
}
