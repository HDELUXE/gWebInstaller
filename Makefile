# Makefile — AngerMU Installer
# Requer: MinGW32, windres, miniz.h no mesmo diretório (ou subdir)
#
# Uso:
#   make          → build release (AngerMU_Installer.exe)
#   make debug    → build com símbolos
#   make clean    → remove artefatos

CXX      := C:/msys6432/mingw32/bin/g++
WINDRES  := C:/msys6432/mingw32/bin/windres

TARGET   := AngerMU_Installer.exe
SRC      := installer.cpp miniz/miniz.c
RES_SRC  := installer.rc
RES_OBJ  := installer.res

CXXFLAGS := -std=c++17 -Wall -Wextra \
            -DUNICODE -D_UNICODE \
            -I. -I./miniz/
 
LDFLAGS  := -mwindows -static \
            -lwininet -lgdiplus -lshell32 \
            -lole32 -luuid \
            -lstdc++ -lgcc -lmingwex -lmsvcrt \
            -static-libgcc -static-libstdc++ \
            -Wl,-Bstatic -lpthread -Wl,-Bdynamic

CXXFLAGS_REL := -O2 -DNDEBUG
CXXFLAGS_DBG := -O0 -g -DDEBUG

# ── Targets ──────────────────────────────────────────────────

all: $(TARGET)

debug: CXXFLAGS += $(CXXFLAGS_DBG)
debug: TARGET := AngerMU_Installer_dbg.exe
debug: $(TARGET)

$(TARGET): $(SRC) $(RES_OBJ)
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_REL) -o $@ $(SRC) $(RES_OBJ) $(LDFLAGS)

$(RES_OBJ): $(RES_SRC) installer.manifest
	$(WINDRES) $(RES_SRC) -O coff -o $(RES_OBJ)

clean:
	del /Q $(TARGET) $(RES_OBJ) 2>nul || rm -f $(TARGET) $(RES_OBJ)

.PHONY: all debug clean
