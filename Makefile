# Build glue for slsteam-moon.
#
# Common invocations:
#   make             Build bin/SLSsteam.so + bin/pattern-refresh on the host.
#                    Use scripts/build.sh --portable for a release-grade
#                    binary with broader glibc compatibility.
#   make clean       Remove all build artefacts.
#   make install     Install onto the host (delegates to setup.sh install).
#   make release     Build portable + package dist/slsteam-moon-linux-<ver>.zip.
#                    Same as scripts/release.sh.
#
# -MMD/-MP keep dependency files in sync so header edits trigger
# recompilation, see https://stackoverflow.com/q/52034997.

# Force g++; clang miscompiles a few hooks.
CXX := g++
.DEFAULT_GOAL := all
STEAMCLIENT ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamclient.so
STEAMUI ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamui.so

libs := $(wildcard lib/*.a)
# Generated, but nothing includes them.
unused_protos := src/sdk/protobufs/steammessages_player.steamclient.pb.cpp \
	src/sdk/protobufs/steammessages_unified_base.steamclient.pb.cpp
srcs := $(filter-out $(unused_protos),$(shell find src/ -type f -iname "*.cpp"))
objs := $(srcs:src/%.cpp=obj/%.o)
deps := $(objs:%.o=%.d)

# -fno-reorder-blocks-and-partition keeps SLSsteam.so about 590 KB smaller.
# Exceptions do not depend on it: an audit module built with block
# partitioning catches its own throws, and the boot loops once blamed on
# ".cold" landing pads came from a second audit module (see setup.sh).
# A log format that does not match its arguments is an error: it runs inside
# Steam, where a bad conversion reads garbage or crashes the client.
WARNFLAGS := -Wall -Wextra -Wpedantic -Werror=format -Werror=format-security
CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 \
            -fno-reorder-blocks-and-partition \
            $(WARNFLAGS) \
            -D_GLIBCXX_USE_CXX11_ABI=0

LDFLAGS := -shared -Wl,--no-undefined -lpthread -ldl

ifeq ($(shell echo $$NATIVE),1)
	CXXFLAGS += -march=native
endif

# Optional speed-ups picked up if installed.
ifeq ($(shell type ccache &> /dev/null && echo "found"),found)
	export PATH := /usr/lib/ccache/bin:$(PATH)
endif
ifeq ($(shell type mold &> /dev/null && echo "found"),found)
	LDFLAGS += -fuse-ld=mold
endif

.PHONY: all build rebuild clean install release test-ascii test-cmwire test-cmclient-loader test-cmclient-session test-clientcompat test-dlcids test-dlc-scope test-dlc-metadata test-config-path test-config-discovery test-lua-import test-steamstub-launch test-ownerwork-policy test-filewatcher-burst test-provision-notice test-mtvar-contains test-synthmark test-appinfostate test-pattern-catalog test-pattern-cache test-pattern-refresh test-process-lock test-process-runtime test-atomic-file test-cache-pair test-appinfo-transaction test-appinfo-reload test-audit-symbols test-audit-eh test-audit-policy test-memhlp-target test-memhlp-prologue test-memhlp-pic test-utils-sha test-provision-cache test-provision-refresh test-provision-result test-provision-terminal test-pending-proton test-provision-schedule test-provision-pass test-runtime-dependencies test-thread-start test-steamstub-warmup test-steamstub-launchopt test-boundedexecutor test-steamless-prewarm test-depotkey-scope test-curl-timeout test-manifest-index test-manifestselection test-manifeststore-io test-hotreload-inputs test-hotreload-package test-ownerqueue test-hotreload-capabilities test-libraryremoval test-pics test-yaml-runtime test-manifestpin-patterns test-optional-locators test-patternscan test-fakeappid-presence test-fakeappid-wiring test-ticket test-familyshare test-familyshare-wiring
.NOTPARALLEL: clean rebuild
.PHONY: uninstall clean-libs clean-tools schema-grabber ticket-grabber
.PHONY: test-library-dates test-library-dates-hook test-depot-overrides
.PHONY: test-vftableinfo-registry test-owner-frame-wiring
.PHONY: test-achievements test-achievement-scope test-stats-audit
.PHONY: test-stats-provenance
.PHONY: test-manifestdonor test-manifestdonor-runtime test-manifestfetch
.PHONY: test-reconcilepin-scan
.PHONY: test-depotkey-recv-policy
.PHONY: test-private-apps test-config-settings test-config-tolerance test-config-create test-plugin-config test-curl-child test-legacy-cdkey test-utils-number test-launch-options

test-manifestdonor:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_manifestdonor.cpp -o /tmp/test_manifestdonor
	/tmp/test_manifestdonor

test-manifestfetch:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_manifestfetch.cpp -o /tmp/test_manifestfetch
	/tmp/test_manifestfetch
test-manifestdonor-runtime: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o obj/feats/librarydates.o obj/ownerwork.o \
		obj/feats/manifestdonor.o obj/sdk/protobufs/encrypted_app_ticket.pb.o \
		obj/sdk/protobufs/steammessages_clientserver.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_manifestdonor_runtime.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_manifestdonor_runtime
	/tmp/test_manifestdonor_runtime

test-stats-provenance:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections tools/test_stats_provenance.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_stats_provenance
	/tmp/test_stats_provenance

test-familyshare: obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections tools/test_familyshare.cpp \
		src/sdk/CNetPacket.cpp $^ lib/libprotobuf.a \
		-Wl,--gc-sections -pthread -o /tmp/test_familyshare
	/tmp/test_familyshare

test-familyshare-wiring:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_familyshare_wiring.cpp -o /tmp/test_familyshare_wiring
	/tmp/test_familyshare_wiring


test-achievements:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -isystem include -pthread tools/test_achievements.cpp -o /tmp/test_achievements
	/tmp/test_achievements

test-achievement-lookup:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src -pthread \
		tools/test_achievement_lookup.cpp src/feats/achievement_lookup.cpp \
		-o /tmp/test_achievement_lookup
	/tmp/test_achievement_lookup
test-achievement-scope: obj/sdk/protobufs/steammessages_clientserver_userstats.pb.o obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections \
		tools/test_achievement_scope.cpp src/feats/achievements.cpp src/feats/stats_policy.cpp \
		src/sdk/CNetPacket.cpp $^ lib/libprotobuf.a -Wl,--gc-sections -pthread \
		-o /tmp/test_achievement_scope
	/tmp/test_achievement_scope

test-fakeappid-presence: obj/sdk/protobufs/encrypted_app_ticket.pb.o \
                          obj/sdk/protobufs/steammessages_base.pb.o \
                          obj/sdk/protobufs/steammessages_clientserver.pb.o \
                          obj/sdk/protobufs/steammessages_clientserver_2.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		-ffunction-sections -fdata-sections tools/test_fakeappid_presence.cpp \
		src/feats/fakeappid.cpp src/sdk/CNetPacket.cpp $^ lib/libprotobuf.a \
		-Wl,--gc-sections -pthread -o /tmp/test_fakeappid_presence
	/tmp/test_fakeappid_presence

test-fakeappid-wiring:
	$(CXX) -m32 -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_fakeappid_wiring.cpp -o /tmp/test_fakeappid_wiring
	/tmp/test_fakeappid_wiring

test-ticket:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -Werror -I include -I src \
		tools/test_ticket.cpp -o /tmp/test_ticket
	/tmp/test_ticket

test-process-runtime:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic \
		-ffunction-sections -fdata-sections -I include -I src \
		tools/test_process_runtime.cpp src/process.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_process_runtime
	/tmp/test_process_runtime

test-stats-audit: bin/SLSsteam.so
	$(CXX) -m32 -std=c++20 -rdynamic tools/test_stats_audit.cpp -ldl -o /tmp/test_stats_audit
	/tmp/test_stats_audit
	LD_AUDIT=$(CURDIR)/bin/SLSsteam.so /tmp/test_stats_audit audit

all: build
build: bin/SLSsteam.so bin/pattern-refresh
rebuild: clean build

bin/SLSsteam.so: $(objs) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

PATTERN_PUBLIC_KEY_HEX = $(shell tr -d '[:space:]' < res/pattern-public-key.hex 2>/dev/null)

# Pre-launch metadata refresher. This is a native host executable, not a
# Steam-loaded i386 object: HTTPS, Ed25519, and cache I/O finish before Steam
# starts and never run in LD_AUDIT preinit.
bin/pattern-refresh: tools/pattern-refresh/main.cpp \
                     tools/pattern-refresh/catalog.cpp \
                     tools/pattern-refresh/catalog.hpp \
                     src/pattern_catalog.cpp src/pattern_catalog.hpp \
                     res/pattern-public-key.hex
	@test -n "$(PATTERN_PUBLIC_KEY_HEX)" || { echo "missing pattern public key" >&2; exit 1; }
	@mkdir -p bin
	$(CXX) -O2 -std=c++20 -Wall -Wextra -Wpedantic \
		-I src -I tools/pattern-refresh \
		-DPATTERN_PUBLIC_KEY_HEX='"$(PATTERN_PUBLIC_KEY_HEX)"' \
		tools/pattern-refresh/main.cpp tools/pattern-refresh/catalog.cpp \
		src/pattern_catalog.cpp -lcurl -lcrypto -lpthread -o $@

# .NET helpers, not part of the default build.
schema-grabber:
	$(MAKE) -C tools/schema-grabber

ticket-grabber:
	$(MAKE) -C tools/ticket-grabber

-include $(deps)
obj/config.o: src/config.cpp res/config.yaml
	$(shell ./embed-config.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/%.o : src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

clean: clean-libs clean-tools
clean-libs:
	rm -rvf \
		"obj/" \
		"bin/" \
		"dist/" \
		"zips/" \
		"tools/ticket-grabber/bin" \
		"tools/ticket-grabber/obj" \
		"tools/schema-grabber/bin" \
		"tools/schema-grabber/obj"

clean-tools:
	-$(MAKE) -C tools/schema-grabber clean
	-$(MAKE) -C tools/ticket-grabber clean

install:
	sh setup.sh install

uninstall:
	sh setup.sh uninstall

# Unit test for cmwire.hpp's protobuf-backed helpers (Task 3).  Links the
# already-compiled protobuf objects + libprotobuf.a, matching the
# main build's 32-bit + pre-C++11 ABI so the static lib is compatible.
# The pure framing tests (Tasks 1-2) also build standalone without this:
#   g++ -std=c++20 -I include tools/test_cmwire.cpp -o /tmp/test_cmwire
test-cmwire: obj/sdk/protobufs/steammessages_base.pb.o \
             obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
             obj/sdk/protobufs/steammessages_clientserver_login.pb.o \
             obj/sdk/protobufs/steammessages_clientserver.pb.o \
             obj/sdk/protobufs/steammessages_clientserver_2.pb.o \
             obj/sdk/protobufs/steammessages_clientserver_friends.pb.o \
             obj/sdk/protobufs/steammessages_clientserver_userstats.pb.o \
             obj/sdk/protobufs/encrypted_app_ticket.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -DCMWIRE_PROTOBUF \
		-I include tools/test_cmwire.cpp $^ lib/libprotobuf.a \
		-lpthread -o /tmp/test_cmwire
	/tmp/test_cmwire

# CmClient against scripted CMs; tools/fake_libcurl.cpp stands in for libcurl.
test-cmclient-session: obj/feats/cmclient.o obj/log.o obj/config.o obj/globals.o \
		obj/filewatcher.o obj/feats/librarydates.o \
		obj/sdk/protobufs/steammessages_base.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_login.pb.o \
		obj/sdk/protobufs/steammessages_clientserver.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_2.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_friends.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_userstats.pb.o \
		obj/sdk/protobufs/encrypted_app_ticket.pb.o
	mkdir -p /tmp/fakecurl
	$(CXX) -m32 -std=c++20 -shared -fPIC -Wl,-soname,libcurl.so.4 \
		tools/fake_libcurl.cpp -o /tmp/fakecurl/libcurl.so.4
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_cmclient_session.cpp \
		$(filter obj/%.o,$^) /tmp/fakecurl/libcurl.so.4 -Wl,-rpath,/tmp/fakecurl \
		lib/libprotobuf.a lib/libyaml-cpp.a -lpthread -ldl \
		-o /tmp/test_cmclient_session
	/tmp/test_cmclient_session
test-cmclient-loader:
	$(CXX) -std=c++20 -ffunction-sections -fdata-sections -I include -I src \
		tools/test_cmclient_loader.cpp src/feats/cmclient.cpp \
		-Wl,--gc-sections -ldl -pthread -o /tmp/test_cmclient_loader
	/tmp/test_cmclient_loader

test-clientcompat:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_clientcompat.cpp src/sdk/IClientCompat.cpp \
		-o /tmp/test_clientcompat
	/tmp/test_clientcompat

test-dlcids:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_dlcids.cpp -o /tmp/test_dlcids
	/tmp/test_dlcids

test-ascii:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_ascii.cpp -o /tmp/test_ascii
	/tmp/test_ascii

test-dlc-scope:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -I src -I include \
		tools/test_dlc_scope.cpp src/feats/dlc.cpp \
		src/feats/managed_dlc_scope.cpp -pthread \
		-o /tmp/test_dlc_scope
	/tmp/test_dlc_scope

test-dlc-metadata:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -I include -I src \
		tools/test_dlc_metadata.cpp src/feats/dlc_metadata.cpp \
		lib/libyaml-cpp.a -o /tmp/test_dlc_metadata
	/tmp/test_dlc_metadata

test-config-path:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_config_path.cpp -o /tmp/test_config_path
	/tmp/test_config_path

test-config-discovery:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_config_discovery.cpp -o /tmp/test_config_discovery
	/tmp/test_config_discovery
test-reconcilepin-scan:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_reconcilepin_scan.cpp -o /tmp/test_reconcilepin_scan
	/tmp/test_reconcilepin_scan

test-filewatcher-burst:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread -I src \
		tools/test_filewatcher_burst.cpp -o /tmp/test_filewatcher_burst
	/tmp/test_filewatcher_burst

test-provision-notice:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_provision_notice.cpp -o /tmp/test_provision_notice
	/tmp/test_provision_notice

test-mtvar-contains:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread -I src \
		tools/test_mtvar_contains.cpp -o /tmp/test_mtvar_contains
	/tmp/test_mtvar_contains

test-synthmark:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_synthmark.cpp -o /tmp/test_synthmark
	/tmp/test_synthmark

test-appinfostate:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread -I src -I include \
		tools/test_appinfostate.cpp -o /tmp/test_appinfostate
	/tmp/test_appinfostate

test-pattern-catalog:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_pattern_catalog.cpp src/pattern_catalog.cpp \
		-o /tmp/test_pattern_catalog
	/tmp/test_pattern_catalog

test-pattern-cache:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_pattern_cache.cpp src/pattern_cache.cpp \
		-o /tmp/test_pattern_cache
	/tmp/test_pattern_cache

test-appinfo-reload:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_appinforeload.cpp -o /tmp/test_appinforeload
	/tmp/test_appinforeload

test-pattern-refresh:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I tools -I src \
		tools/test_pattern_refresh.cpp tools/pattern-refresh/catalog.cpp \
		src/pattern_catalog.cpp -lcurl -lcrypto -lpthread \
		-o /tmp/test_pattern_refresh
	/tmp/test_pattern_refresh

test-audit-symbols:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_audit_symbols.cpp -o /tmp/test_audit_symbols
	/tmp/test_audit_symbols

test-audit-eh:
	$(CXX) $(CXXFLAGS) tools/test_audit_eh.cpp -o /tmp/test_audit_eh.so $(LDFLAGS)
	printf 'int main(void) { return 0; }\n' | $(CC) -m32 -x c - -o /tmp/test_audit_eh_main
	test "$$(LD_AUDIT=/tmp/test_audit_eh.so /tmp/test_audit_eh_main)" = caught

test-audit-policy:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_audit_policy.cpp -o /tmp/test_audit_policy
	/tmp/test_audit_policy

test-depotkey-scope:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_depotkey_scope.cpp -pthread -o /tmp/test_depotkey_scope
	/tmp/test_depotkey_scope

test-depotkey-recv-policy:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_depotkey_recv_policy.cpp -o /tmp/test_depotkey_recv_policy
	/tmp/test_depotkey_recv_policy

test-memhlp-target:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_memhlp_target.cpp -o /tmp/test_memhlp_target
	/tmp/test_memhlp_target

test-memhlp-prologue:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_memhlp_prologue.cpp -o /tmp/test_memhlp_prologue
	/tmp/test_memhlp_prologue

test-memhlp-pic:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_memhlp_pic.cpp -o /tmp/test_memhlp_pic
	/tmp/test_memhlp_pic

test-utils-number:
	$(CXX) -m32 -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_utils_number.cpp src/utils.cpp -o /tmp/test_utils_number
	/tmp/test_utils_number

test-utils-sha:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_utils_sha.cpp src/utils.cpp -o /tmp/test_utils_sha
	/tmp/test_utils_sha

test-provision-cache:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_cache.cpp -o /tmp/test_provision_cache
	/tmp/test_provision_cache

test-provision-refresh:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_refresh.cpp -o /tmp/test_provision_refresh
	/tmp/test_provision_refresh

test-provision-result:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_result.cpp -o /tmp/test_provision_result
	/tmp/test_provision_result

test-provision-terminal:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_terminal.cpp src/feats/provision_terminal.cpp \
		-o /tmp/test_provision_terminal
	/tmp/test_provision_terminal

test-pending-proton:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_pending_proton.cpp -o /tmp/test_pending_proton
	/tmp/test_pending_proton

test-provision-schedule:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_schedule.cpp -o /tmp/test_provision_schedule
	/tmp/test_provision_schedule

test-provision-pass:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_provision_pass.cpp -pthread \
		-o /tmp/test_provision_pass
	/tmp/test_provision_pass

test-runtime-dependencies:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -I src \
		-c src/runtime_dependencies.cpp -o /tmp/runtime_dependencies.o
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -I src \
		tools/test_runtime_dependencies.cpp /tmp/runtime_dependencies.o \
		-Wl,--gc-sections -o /tmp/test_runtime_dependencies
	/tmp/test_runtime_dependencies

test-thread-start:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_thread_start.cpp -o /tmp/test_thread_start
	/tmp/test_thread_start

test-steamstub-warmup:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_steamstub_warmup.cpp -o /tmp/test_steamstub_warmup
	/tmp/test_steamstub_warmup

test-steamstub-launchopt:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_steamstub_launchopt.cpp -o /tmp/test_steamstub_launchopt
	/tmp/test_steamstub_launchopt

test-boundedexecutor:
	$(CXX) -std=c++20 -pthread -Wall -Wextra -Wpedantic -Werror \
		tools/test_boundedexecutor.cpp -o /tmp/test_boundedexecutor
	/tmp/test_boundedexecutor

test-steamless-prewarm:
	bash scripts/test-steamless-prewarm.sh

test-curl-timeout:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_curl_timeout.cpp src/curl.cpp -ldl -pthread \
		-o /tmp/test_curl_timeout
	/tmp/test_curl_timeout

test-curl-child:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_curl_child.cpp src/curl.cpp -ldl -pthread \
		-o /tmp/test_curl_child
	/tmp/test_curl_child

test-manifest-index:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_manifest_index.cpp -o /tmp/test_manifest_index
	/tmp/test_manifest_index

test-manifestselection:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_manifestselection.cpp -o /tmp/test_manifestselection
	/tmp/test_manifestselection

test-manifeststore-io:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_manifeststore_io.cpp -o /tmp/test_manifeststore_io
	/tmp/test_manifeststore_io

test-hotreload-inputs:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_hotreload_inputs.cpp src/feats/provision_terminal.cpp \
		-o /tmp/test_hotreload_inputs
	/tmp/test_hotreload_inputs

test-hotreload-package:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic \
		tools/test_hotreload_package.cpp -o /tmp/test_hotreload_package
	/tmp/test_hotreload_package

test-ownerqueue:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -pthread \
		tools/test_ownerqueue.cpp -o /tmp/test_ownerqueue
	/tmp/test_ownerqueue

test-hotreload-capabilities:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_hotreload_capabilities.cpp -o /tmp/test_hotreload_capabilities
	/tmp/test_hotreload_capabilities

test-libraryremoval: obj/feats/libraryremoval.o
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_libraryremoval.cpp -o /tmp/test_libraryremoval
	/tmp/test_libraryremoval
	@if nm -C obj/feats/libraryremoval.o | grep -E 'google::protobuf|RepeatedField'; then \
		echo "library removal must not mutate Steam protobufs across the audit namespace" >&2; \
		exit 1; \
	fi

test-library-dates:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_library_dates.cpp src/feats/librarydates.cpp -pthread \
		-o /tmp/test_library_dates
	/tmp/test_library_dates

test-owner-frame-wiring:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror \
		tools/test_owner_frame_wiring.cpp -o /tmp/test_owner_frame_wiring
	/tmp/test_owner_frame_wiring

test-vftableinfo-registry:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_vftableinfo_registry.cpp src/vftableinfo.cpp \
		-o /tmp/test_vftableinfo_registry
	/tmp/test_vftableinfo_registry

test-depot-overrides:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_depot_overrides.cpp src/feats/apps.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_depot_overrides
	/tmp/test_depot_overrides

test-config-settings:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_config_settings.cpp src/feats/librarydates.cpp lib/libyaml-cpp.a \
		-Wl,--gc-sections -pthread -o /tmp/test_config_settings
	/tmp/test_config_settings
test-private-apps:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_private_apps.cpp src/feats/apps.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_private_apps
	/tmp/test_private_apps
test-launch-options:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_launch_options.cpp src/feats/apps.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_launch_options
	/tmp/test_launch_options
test-legacy-cdkey:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_legacy_cdkey.cpp src/feats/apps.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_legacy_cdkey
	/tmp/test_legacy_cdkey
test-library-dates-hook:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-fno-reorder-blocks-and-partition -ffunction-sections -fdata-sections \
		-Wall -Wextra -Wpedantic -isystem include -I src \
		tools/test_library_dates_hook.cpp src/feats/apps.cpp src/feats/librarydates.cpp \
		-Wl,--gc-sections -pthread -o /tmp/test_library_dates_hook
	/tmp/test_library_dates_hook

test-pics:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_pics.cpp src/feats/provision_terminal.cpp -o /tmp/test_pics
	/tmp/test_pics

test-process-lock:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_process_lock.cpp -o /tmp/test_process_lock
	/tmp/test_process_lock

test-atomic-file:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -DATOMIC_FILE_TESTING -I src \
		tools/test_atomic_file.cpp -o /tmp/test_atomic_file
	/tmp/test_atomic_file

test-cache-pair:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_cache_pair.cpp -o /tmp/test_cache_pair
	/tmp/test_cache_pair

test-yaml-runtime:
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -I include \
		tools/test_yaml_runtime.cpp lib/libyaml-cpp.a -o /tmp/test_yaml_runtime
	/tmp/test_yaml_runtime

# Non-LTO 32-bit object so the host linker can link the transaction test
# without choking on LTO bytecode from a different toolchain version.
obj/feats/appinfo_vdf_test.o: src/feats/appinfo_vdf.cpp $(deps_early)
	@mkdir -p obj/feats
	$(CXX) -O2 -fno-lto -fPIC -m32 -std=c++20 -fno-reorder-blocks-and-partition \
		-Wall -Wextra -Wpedantic -Wno-error=format-security \
		-D_GLIBCXX_USE_CXX11_ABI=0 -DAPPINFO_VDF_TESTING \
		-I include -isysteminclude -MMD -MP \
		-c src/feats/appinfo_vdf.cpp -o obj/feats/appinfo_vdf_test.o

test-appinfo-transaction: obj/feats/appinfo_vdf_test.o obj/log.o obj/config.o \
		obj/globals.o obj/filewatcher.o obj/api.o \
		obj/feats/depotkey.o obj/feats/librarydates.o \
		obj/ownerwork.o obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -DAPPINFO_VDF_TESTING \
		-I include -isysteminclude tools/test_appinfo_transaction.cpp \
		obj/feats/appinfo_vdf_test.o \
		$(filter-out obj/feats/appinfo_vdf_test.o,$(filter obj/%.o,$^)) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_appinfo_transaction
	/tmp/test_appinfo_transaction

test-config-tolerance: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o \
		obj/feats/librarydates.o obj/ownerwork.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_config_tolerance.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_config_tolerance
	/tmp/test_config_tolerance

test-steamstub-launch: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o \
		obj/feats/librarydates.o obj/ownerwork.o obj/feats/steamstub.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_steamstub_launch.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_steamstub_launch
	/tmp/test_steamstub_launch
test-lua-import: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o \
		obj/feats/librarydates.o obj/ownerwork.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_lua_import.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_lua_import
	/tmp/test_lua_import
test-ownerwork-policy: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o \
		obj/feats/librarydates.o obj/ownerwork.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_ownerwork_policy.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_ownerwork_policy
	/tmp/test_ownerwork_policy
test-config-create: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o \
		obj/feats/librarydates.o obj/ownerwork.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_config_create.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_config_create
	/tmp/test_config_create

test-plugin-config: obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/api.o obj/feats/depotkey.o \
		obj/feats/librarydates.o obj/ownerwork.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 \
		-I include -isysteminclude tools/test_plugin_config.cpp \
		$(filter obj/%.o,$^) \
		lib/libyaml-cpp.a lib/libprotobuf.a -lcrypto -lpthread -ldl \
		-o /tmp/test_plugin_config
	/tmp/test_plugin_config

# Live integration harness for the native CM product-info client (talks
# to real Valve CMs — NOT a unit test).  Links cmclient + its deps.
test-cmclient-live: build
	$(CXX) $(CXXFLAGS) -DCMWIRE_PROTOBUF \
		-I include -isysteminclude tools/test_cmclient_live.cpp \
		obj/feats/cmclient.o obj/log.o obj/config.o obj/globals.o obj/filewatcher.o \
		obj/feats/librarydates.o \
		obj/sdk/protobufs/steammessages_base.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_login.pb.o \
		obj/sdk/protobufs/steammessages_clientserver.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_2.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_friends.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_userstats.pb.o \
		obj/sdk/protobufs/encrypted_app_ticket.pb.o \
		lib/libprotobuf.a lib/libyaml-cpp.a \
		-lpthread -ldl -lcurl -o /tmp/test_cmclient_live

test-manifestpin-patterns:
	$(CXX) -std=c++20 tools/test_manifestpin_patterns.cpp \
		-o /tmp/test_manifestpin_patterns
	/tmp/test_manifestpin_patterns "$(STEAMCLIENT)"

# The two optional locators whose failure only degrades a feature, so a drift
# after a client update would otherwise pass unnoticed.
#
# Point PREV_STEAMCLIENT/PREV_STEAMUI at the previous client's modules to also
# gate the transport: a relaxed signature can be unique on the new build and
# ambiguous (or land on a neighbouring field) on the one users still run, which
# a single-build check cannot see.
test-patternscan:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_patternscan.cpp -o /tmp/test_patternscan
	/tmp/test_patternscan

test-log-rotate:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_log_rotate.cpp -o /tmp/test_log_rotate
	/tmp/test_log_rotate

test-log-format:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I src \
		tools/test_log_format.cpp -o /tmp/test_log_format
	/tmp/test_log_format
	@for c in MISMATCH NONLITERAL CUSTOM; do \
		if $(CXX) -std=c++20 $(WARNFLAGS) -I src -fsyntax-only -DLOG_FORMAT_$$c \
			tools/test_log_format.cpp 2>/dev/null; then \
			echo "FAIL: LOG_FORMAT_$$c compiled"; exit 1; \
		fi; \
		echo "ok: LOG_FORMAT_$$c rejected"; \
	done
test-vftable-anchors:
	$(CXX) -O2 -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src \
		tools/test_vftable_anchors.cpp src/decompiler.cpp src/vftableinfo.cpp \
		src/utils.cpp src/process.cpp lib/liblibmem.a -ldl -lpthread \
		-o /tmp/test_vftable_anchors
	/tmp/test_vftable_anchors "$(STEAMCLIENT)" "$(STEAMUI)"
test-optional-locators:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_optional_locators.cpp -o /tmp/test_optional_locators
	/tmp/test_optional_locators "$(STEAMCLIENT)" "$(STEAMUI)" src/patterns.cpp
ifneq ($(strip $(PREV_STEAMCLIENT)$(PREV_STEAMUI)),)
	@test -n "$(strip $(PREV_STEAMCLIENT))" && test -n "$(strip $(PREV_STEAMUI))" \
		|| { echo "set both PREV_STEAMCLIENT and PREV_STEAMUI" >&2; exit 2; }
	@echo "==> previous client transport check"
	/tmp/test_optional_locators "$(PREV_STEAMCLIENT)" "$(PREV_STEAMUI)" src/patterns.cpp
endif

release:
	bash scripts/release.sh
