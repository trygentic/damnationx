# DamnationX desktop launcher and the desktop game it starts (macOS): HD art through native Dawn,
# Multi Player through the default lobby (DEVILUTIONX_DEFAULT_LOBBY_ORIGIN, https://lobby.damnationx.net).
#
#   make -C tools/launcher run        build whatever is missing, then start the launcher
#   make -C tools/launcher game       configure + build the desktop game into build-desktop/
#   make -C tools/launcher launcher   build the launcher (cargo, release)
#   make -C tools/launcher dev        private dev build: game with the trainer + dev console (build-dev/),
#                                     the trainer GUI, and a launcher with a "Trainer" button
#
# In the launcher: "Game files..." picks DIABDAT.MPQ, "HD folder..." picks the HD art (optional).
# The first configure fetches and builds Dawn (needs git, Python 3, network; ~1.5 GB) into DAWN_ROOT.
# Put the builds on another disk with e.g.
#   make -C tools/launcher run BUILD_DIR=/Volumes/ssd/build-desktop DAWN_ROOT=/Volumes/ssd/dawn

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILD_DIR ?= $(ROOT)/build-desktop
DAWN_ROOT ?= $(BUILD_DIR)/dawn
GAME := $(BUILD_DIR)/devilutionx.app/Contents/MacOS/devilutionx
# Ask cargo where it puts the binary: CARGO_TARGET_DIR or a cargo config can move it out of target/.
LAUNCHER = $(shell cargo metadata --format-version 1 --no-deps --manifest-path "$(ROOT)/tools/launcher/Cargo.toml" \
	| sed -n 's/.*"target_directory":"\([^"]*\)".*/\1/p')/release/launcher
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

CMAKE_FLAGS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DUSE_SDL3=ON \
	-DDEVILUTIONX_WEBGPU=ON \
	-DDEVILUTIONX_DAWN_BUILD_ROOT="$(DAWN_ROOT)" \
	-DNONET=OFF \
	-DDEVILUTIONX_LOBBY=ON \
	-DDEVILUTIONX_DEFAULT_LOBBY_ORIGIN=https://lobby.damnationx.net \
	-DPACKET_ENCRYPTION=ON \
	-DDISABLE_ZERO_TIER=ON \
	-DDISABLE_TCP=ON \
	-DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF \
	-DDEVILUTIONX_SYSTEM_GOOGLETEST=OFF \
	-DDEVILUTIONX_SYSTEM_BENCHMARK=OFF \
	-DDEVILUTIONX_DISABLE_EXCEPTIONS=OFF \
	-DDISCORD_INTEGRATION=OFF \
	-DBUILD_TESTING=OFF

.PHONY: run game launcher dev

game:
	cmake -S "$(ROOT)" -B "$(BUILD_DIR)" -G "Unix Makefiles" $(CMAKE_FLAGS)
	cmake --build "$(BUILD_DIR)" --target devilutionx -j $(JOBS)

launcher:
	cargo build --release --manifest-path "$(ROOT)/tools/launcher/Cargo.toml"

run: game launcher
	"$(LAUNCHER)" "$(GAME)"

# Never ship these: DEVILUTIONX_TRAINER implies DEBUG (cheats, dev console), and the version string says Debug.
DEV_BUILD_DIR ?= $(ROOT)/build-dev
TRAINER_GUI := $(ROOT)/tools/trainer/gui/.build/release/DamnationTrainer

dev:
	cmake -S "$(ROOT)" -B "$(DEV_BUILD_DIR)" -G "Unix Makefiles" $(CMAKE_FLAGS) -DDEVILUTIONX_TRAINER=ON
	cmake --build "$(DEV_BUILD_DIR)" --target devilutionx -j $(JOBS)
	swift build -c release --package-path "$(ROOT)/tools/trainer/gui"
	cargo build --release --features trainer --manifest-path "$(ROOT)/tools/launcher/Cargo.toml" --target-dir "$(ROOT)/tools/launcher/target-dev"
	"$(ROOT)/tools/launcher/target-dev/release/launcher" "$(DEV_BUILD_DIR)/devilutionx.app/Contents/MacOS/devilutionx" "$(TRAINER_GUI)"
