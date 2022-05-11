VER = 1.2.23
GIT_COMMIT = ae0b91e4b5e9da2c0e97f2eb07934f9519408609  
GIT_DATE = "Mon Apr 4 13:03:30 2022 -0500"

define BUGS
all array buffers reinitialized with \0
endef

##############################################
##############################################
### DO NOT CHANGE ANYTHING AFTER THIS LINE ###
##############################################
##############################################
export BUGS
PRODUCT = RAZOR
TIME_STAMP = `date +'%m%d%Y'`
VER2 = $(subst .,_,$(VER))
RELEASE_DATE = $(shell date --iso=seconds)
MD5SUM = md5sum
ZIP = zip
MKDIR = mkdir
ECHO = echo
TAR = tar
RM = rm
CP = cp
MV = mv
CP_OPTIONS = -r
RM_OPTIONS = -rf 
TAR_OPTIONS = -zcvf 

BETA_FOLDER = /cygdrive/U/MODULE/PROG_FILES/Razor/Beta
RELEASE_FOLDER = ../Release
COPY_FOLDER = Release_V$(VER2)_$(TIME_STAMP)
ZIPPED_FOLDER = $(COPY_FOLDER).zip
MD5_FILES = $(RELEASE_FOLDER)/pdi_razor_firmware.ais $(RELEASE_FOLDER)/PDI_Razor.out $(RELEASE_FOLDER)/PDI_Razor.bin $(RELEASE_FOLDER)/PDI_Razor.map
CUSTOMER_FILES = $(MD5_FILES) Makefile $(RELEASE_FOLDER)/boot.cfg
RELEASE_NOTE_FILE = $(RELEASE_FOLDER)/RELEASE_NOTE.txt

CLEAN_THESE = $(RELEASE_FOLDER)/*.zip $(RELEASE_FOLDER)/*.txt $(RELEASE_FOLDER)/RAZOR_* Release*

LINE0 = "-----------------------------------------------------"
LINE1 = "RELEASE SUMMARY"
LINE3 = "PRODUCT      : $(PRODUCT)"
LINE4 = "VERSION      : $(VER)"
LINE5 = "RELEASE DATE : $(RELEASE_DATE)"
LINE8 = "BUGFIXES & CHANGES"
LINE11 = "MD5 HASHES"
LINE14 = "GIT VERSION CONTROL"

all	: clean create_dirs md5hash copy_files

create_dirs:
	$(MKDIR) -p $(COPY_FOLDER)/support

copy_files:
	$(CP) $(CUSTOMER_FILES) $(COPY_FOLDER)/support
	$(CP) $(RELEASE_FOLDER)/pdi_razor_firmware.ais $(RELEASE_FOLDER)/RELEASE_NOTE.txt $(COPY_FOLDER)
	
compress:
	$(ZIP) $(ZIPPED_FOLDER) $(COPY_FOLDER)
	$(MV) $(ZIPPED_FOLDER) $(COPY_FOLDER)/support

md5hash	:
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE1) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE3) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE4) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE5) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE8) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "$$BUGS" >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE14) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "COMMIT: "$(GIT_COMMIT) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "DATE: "$(GIT_DATE) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) "" 		 >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE11) >> $(RELEASE_NOTE_FILE)
	@$(ECHO) $(LINE0) >> $(RELEASE_NOTE_FILE)
	$(MD5SUM) $(MD5_FILES) >> $(RELEASE_NOTE_FILE)

beta: all
	$(CP) -r $(COPY_FOLDER) $(BETA_FOLDER)

clean:
	@$(RM) $(RM_OPTIONS) $(CLEAN_THESE)
