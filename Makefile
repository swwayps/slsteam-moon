# Build glue for slsteam-moon.
#
# Common invocations:
#   make             Build bin/SLSsteam.so + bin/library-inject.so on the host.
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
STEAMCLIENT ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamclient.so
STEAMUI ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamui.so

libs := $(wildcard lib/*.a)
srcs := $(shell find src/ -type f -iname "*.cpp")
objs := $(srcs:src/%.cpp=obj/%.o)
deps := $(objs:%.o=%.d)

# -fno-reorder-blocks-and-partition: keep exception landing pads in the hot
# partition. With block partitioning (implied at -O2+), a throw's handler can
# land in the ".cold" section and the call-site table fails to route to it, so
# even catch(...) is bypassed and the client aborts (see config.hpp / the
# FakeWalletBalance + malformed-config boot loops). We avoid throwing across
# this boundary in the config path regardless, but pinning the landing pads
# makes every remaining try/catch a reliable backstop.
CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 \
            -fno-reorder-blocks-and-partition \
            -Wall -Wextra -Wpedantic -Wno-error=format-security \
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

.PHONY: all build rebuild clean install release test-cmwire test-cmclient-loader test-dlcids test-dlc-scope test-dlc-metadata test-config-path test-config-discovery test-synthmark test-pattern-catalog test-pattern-cache test-pattern-refresh test-process-lock test-atomic-file test-cache-pair test-appinfo-transaction test-appinfo-reload test-audit-symbols test-audit-policy test-memhlp-target test-memhlp-prologue test-memhlp-pic test-utils-sha test-provision-cache test-provision-refresh test-provision-result test-provision-terminal test-pending-proton test-provision-schedule test-provision-pass test-runtime-dependencies test-thread-start test-steamstub-warmup test-boundedexecutor test-steamless-prewarm test-depotkey-scope test-curl-timeout test-manifest-index test-manifestselection test-manifeststore-io test-hotreload-inputs test-hotreload-package test-ownerqueue test-hotreload-capabilities test-libraryremoval test-pics test-prewarm-backoff test-yaml-runtime test-manifestpin-patterns test-optional-locators
.NOTPARALLEL: clean rebuild

all: build
build: bin/SLSsteam.so bin/library-inject.so bin/pattern-refresh
rebuild: clean build

bin/SLSsteam.so: $(objs) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Audit-side helper that redirects libcurl loading to a system copy.
# Loaded ahead of SLSsteam.so via $LD_AUDIT.
bin/library-inject.so: tools/library-inject/main.cpp
	@mkdir -p bin
	$(CXX) -O3 -m32 -fPIC -shared -std=c++20 $< -o $@

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

-include $(deps)
obj/update.o: src/update.cpp res/version.txt
	$(shell ./embed-version.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/config.o: src/config.cpp res/config.yaml
	$(shell ./embed-config.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/%.o : src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

clean:
	rm -rf obj/ bin/ dist/

install:
	sh setup.sh install

# Unit test for cmwire.hpp's protobuf-backed helpers (Task 3).  Links the
# already-compiled protobuf objects + libprotobuf-lite.a, matching the
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
		-I include tools/test_cmwire.cpp $^ lib/libprotobuf-lite.a \
		-lpthread -o /tmp/test_cmwire
	/tmp/test_cmwire

test-cmclient-loader:
	$(CXX) -std=c++20 -ffunction-sections -fdata-sections -I include -I src \
		tools/test_cmclient_loader.cpp src/feats/cmclient.cpp \
		-Wl,--gc-sections -ldl -pthread -o /tmp/test_cmclient_loader
	/tmp/test_cmclient_loader

test-dlcids:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_dlcids.cpp -o /tmp/test_dlcids
	/tmp/test_dlcids

test-dlc-scope:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_dlc_scope.cpp src/feats/dlc.cpp -pthread \
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

test-synthmark:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src -I include \
		tools/test_synthmark.cpp -o /tmp/test_synthmark
	/tmp/test_synthmark

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

test-audit-policy:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_audit_policy.cpp -o /tmp/test_audit_policy
	/tmp/test_audit_policy

test-depotkey-scope:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include \
		tools/test_depotkey_scope.cpp -pthread -o /tmp/test_depotkey_scope
	/tmp/test_depotkey_scope

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

test-pics:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I include -I src \
		tools/test_pics.cpp src/feats/provision_terminal.cpp -o /tmp/test_pics
	/tmp/test_pics

test-prewarm-backoff:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_prewarm_backoff.cpp -o /tmp/test_prewarm_backoff
	/tmp/test_prewarm_backoff

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
		obj/globals.o obj/filewatcher.o obj/update.o obj/api.o \
		obj/feats/depotkey.o obj/feats/manifestid.o \
		obj/ownerwork.o obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_base.pb.o
	$(CXX) -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -DAPPINFO_VDF_TESTING \
		-I include -isysteminclude tools/test_appinfo_transaction.cpp \
		obj/feats/appinfo_vdf_test.o \
		$(filter-out obj/feats/appinfo_vdf_test.o,$(filter obj/%.o,$^)) \
		lib/libyaml-cpp.a lib/libprotobuf-lite.a -lcrypto -lpthread -ldl \
		-o /tmp/test_appinfo_transaction
	/tmp/test_appinfo_transaction

# Live integration harness for the native CM product-info client (talks
# to real Valve CMs — NOT a unit test).  Links cmclient + its deps.
test-cmclient-live: build
	$(CXX) $(CXXFLAGS) -DCMWIRE_PROTOBUF \
		-I include -isysteminclude tools/test_cmclient_live.cpp \
		obj/feats/cmclient.o obj/log.o obj/config.o obj/globals.o obj/update.o obj/filewatcher.o \
		obj/sdk/protobufs/steammessages_base.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_appinfo.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_login.pb.o \
		obj/sdk/protobufs/steammessages_clientserver.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_2.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_friends.pb.o \
		obj/sdk/protobufs/steammessages_clientserver_userstats.pb.o \
		obj/sdk/protobufs/encrypted_app_ticket.pb.o \
		lib/libprotobuf-lite.a lib/libyaml-cpp.a \
		-lpthread -ldl -lcurl -o /tmp/test_cmclient_live

test-manifestpin-patterns:
	$(CXX) -std=c++20 tools/test_manifestpin_patterns.cpp \
		-o /tmp/test_manifestpin_patterns
	/tmp/test_manifestpin_patterns "$(STEAMCLIENT)"

# The two optional locators whose failure only degrades a feature, so a drift
# after a client update would otherwise pass unnoticed.
test-optional-locators:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -I src \
		tools/test_optional_locators.cpp -o /tmp/test_optional_locators
	/tmp/test_optional_locators "$(STEAMCLIENT)" "$(STEAMUI)" src/patterns.cpp

release:
	bash scripts/release.sh
