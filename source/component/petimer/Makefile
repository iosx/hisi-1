#===============================================================================
# export variable
#===============================================================================
ifeq ($(CFG_HI_EXPORT_FLAG),)
SDK_DIR ?= $(CURDIR)/../../..

include $(SDK_DIR)/cfg.mak
include $(SDK_DIR)/base.mak
endif

TARGET := petimer
#===============================================================================
# local variable
#===============================================================================
CFLAGS += $(CFG_HI_CFLAGS)
CFLAGS += -I$(SDK_DIR)/source/common/include
CFLAGS += -I$(CURDIR)/include

LIBS := lib$(TARGET)

#===============================================================================
# rules
#===============================================================================
.PHONY: all clean install uninstall

ifdef CFG_HI_STATIC_LIB_ONLY
all:$(LIBS).a
else
all:$(LIBS).so $(LIBS).a
endif

clean:
	$(AT)rm -rf $(LIBS).a
	$(AT)rm -rf $(LIBS).so
	$(AT)rm -rf src/*.o

install: all
	$(AT)cp -af $(LIBS).a  $(STATIC_LIB_DIR)/
ifndef CFG_HI_STATIC_LIB_ONLY
	$(AT)cp -af $(LIBS).so $(SHARED_LIB_DIR)/
endif
	$(AT)cp -rf  ./include/* $(INCLUDE_DIR)/

uninstall:
	$(AT)rm -rf $(STATIC_LIB_DIR)/$(LIBS).a
	$(AT)rm -rf $(SHARED_LIB_DIR)/$(LIBS).so
	$(AT)rm -rf $(INCLUDE_DIR)/petimer.h


source =  $(CURDIR)/src/hi_svr_timer.c
source += $(CURDIR)/src/peabstm.c
source += $(CURDIR)/src/pereltm.c
source += $(CURDIR)/src/petimer.c
source += $(CURDIR)/src/tsklet.c

objs=$(source:.c=.o)
	
$(LIBS).a: $(objs)
	$(AR) -rc -o $@ $^

$(LIBS).so: ${objs}
	$(CC) -shared -o $@ $^

%.o: %.c
	@echo cc: $^
	$(AT)$(CC) $(CFLAGS) -c $^ -o "$(CURDIR)/src/$(*F).o"
