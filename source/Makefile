# MX Bikes Ragdoll
#
#   make test   -> compile et lance les tests du moteur physique (Linux / WSL / MSYS2)
#   make dlo    -> compile le plugin Windows build/MXRP_Ragdoll.dlo (MinGW-w64)
#   make clean

CC       ?= gcc
MINGW_CC ?= x86_64-w64-mingw32-gcc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra

CORE_SRC = src/ragdoll.c src/config.c
CORE_HDR = src/ragdoll.h src/config.h

BUILD = build

.PHONY: all test dlo clean

all: test dlo

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_ragdoll: tests/test_ragdoll.c $(CORE_SRC) $(CORE_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_ragdoll.c $(CORE_SRC) -lm

$(BUILD)/test_drive: tests/test_drive.c src/rider_skeleton_beta21e.inc src/drive.c src/drive.h src/walk.c src/walk.h src/terrain.c src/terrain.h src/mocap_walk.inc $(CORE_SRC) $(CORE_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_drive.c src/drive.c src/walk.c src/terrain.c $(CORE_SRC) -lm

test: $(BUILD)/test_ragdoll $(BUILD)/test_drive
	./$(BUILD)/test_ragdoll
	./$(BUILD)/test_drive

# Un plugin MX Bikes est une DLL 64 bits renommée en .dlo
WIN_SRC = src/plugin.c src/hook.c src/skelscan.c src/drive.c src/walk.c src/terrain.c src/glcam.c src/camhook.c src/bike.c src/padinject.c src/equip.c src/replay.c src/rclock.c src/net.c src/kbhook.c src/objects.c src/lang.c src/uiswitch.c src/uipatch.c src/inflate.c src/ridepose.c src/players.c src/cones.c src/bales.c src/profsave.c src/padstyle.c src/padicon.c src/ramps.c src/gamever.c src/groundhook.c src/update.c
WIN_HDR = src/mxb_api.h src/hook.h src/skelscan.h src/drive.h src/walk.h src/terrain.h src/glcam.h src/mocap_walk.inc src/camhook.h src/bike.h src/padinject.h src/rider_skeleton_beta21e.inc src/replay.h src/rclock.h src/net.h src/kbhook.h src/objects.h src/players.h src/ramps.h src/ramp_model.inc src/bales.h src/bale_model.inc src/furn_model.inc

$(BUILD)/MXRP_Ragdoll.dlo: $(WIN_SRC) $(WIN_HDR) $(CORE_SRC) $(CORE_HDR) | $(BUILD)
	$(MINGW_CC) $(CFLAGS) -shared -static-libgcc -s -o $@ $(WIN_SRC) $(CORE_SRC) -lm -ldxguid -lws2_32 -liphlpapi -lopengl32 -lgdi32 -lwinhttp -lbcrypt

dlo: $(BUILD)/MXRP_Ragdoll.dlo

clean:
	rm -rf $(BUILD)

# installeur à donner aux amis : make installer
installer: build/MXRP_Ragdoll.dlo
	x86_64-w64-mingw32-windres -DRC_PLUGIN="\\\"$$(grep -o 'PLUGIN_VERSION "[^"]*"' src/plugin.c | cut -d'"' -f2)\\\"" installer/installer.rc -O coff -o build/installer.res
	x86_64-w64-mingw32-gcc -O2 -Wall -s -mwindows -static -DVER="\"$$(grep -o 'PLUGIN_VERSION "[^"]*"' src/plugin.c | cut -d'"' -f2)\"" installer/installer.c src/inflate.c build/installer.res -o build/MXRP_Ragdoll_Installer.exe -lshell32 -lole32 -ladvapi32
	cp build/MXRP_Ragdoll_Installer.exe "build/MXRP Ragdoll $$(grep -o 'PLUGIN_VERSION "[^"]*"' src/plugin.c | cut -d'"' -f2).exe"
.PHONY: installer
# installeur nommé pour une release (ex. : make installer-release NAME="MXRP Beta 2")
installer-release: build/MXRP_Ragdoll.dlo
	x86_64-w64-mingw32-windres -DRC_NAME="\\\"$(NAME)\\\"" -DRC_PLUGIN="\\\"$$(grep -o 'PLUGIN_VERSION "[^"]*"' src/plugin.c | cut -d'"' -f2)\\\"" installer/installer.rc -O coff -o build/installer.res
	x86_64-w64-mingw32-gcc -O2 -Wall -s -mwindows -static -DVER="\"$(NAME)\"" -DTITLE_NAME="\"$(NAME) ($$(grep -o 'PLUGIN_VERSION "[^"]*"' src/plugin.c | cut -d'"' -f2))\"" installer/installer.c src/inflate.c build/installer.res -o "build/$(NAME).exe" -lshell32 -lole32 -ladvapi32
.PHONY: installer-release
