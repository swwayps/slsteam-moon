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

.PHONY: all build rebuild clean install release test-cmwire
.NOTPARALLEL: clean rebuild

all: build
build: bin/SLSsteam.so bin/library-inject.so
rebuild: clean build

bin/SLSsteam.so: $(objs) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# Audit-side helper that redirects libcurl loading to a system copy.
# Loaded ahead of SLSsteam.so via $LD_AUDIT.
bin/library-inject.so: tools/library-inject/main.cpp
	@mkdir -p bin
	$(CXX) -O3 -m32 -fPIC -shared -std=c++20 $< -o $@

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

release:
	bash scripts/release.sh
