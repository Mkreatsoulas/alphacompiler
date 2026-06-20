WIN_FLEXBISON_DIR := $(LOCALAPPDATA)/Microsoft/WinGet/Packages/WinFlexBison.win_flex_bison_Microsoft.Winget.Source_8wekyb3d8bbwe

ifeq ($(OS),Windows_NT)
    FLEX  := "$(WIN_FLEXBISON_DIR)/win_flex.exe"
    BISON := "$(WIN_FLEXBISON_DIR)/win_bison.exe"
    LFLAGS :=
    CC_OUT_COMP := compiler.exe
    CC_OUT_AVM  := avm.exe
else
    FLEX  := flex
    BISON := bison
    LFLAGS := -lfl
    CC_OUT_COMP := compiler
    CC_OUT_AVM  := avm
endif

all: compiler avm

compiler:
	$(FLEX) --outfile=scanner.c scanner.l
	$(BISON) --defines=parser.h --output=parser.c parser.y
	gcc scanner.c parser.c symtable.c quads.c target_code.c -o $(CC_OUT_COMP) $(LFLAGS)

avm:
	gcc avm.c -o $(CC_OUT_AVM) -lm

clean:
	rm -f scanner.c parser.c parser.h compiler compiler.exe avm avm.exe quads.txt *.abc *.abc.txt
