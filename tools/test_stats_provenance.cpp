// Exercise the real boot append helper, including its provenance updates.
#include "../src/feats/packagepatch.cpp"
#include <cstdio>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

int main()
{
	int failures = 0;
	auto check = [&](bool ok, const char* text) {
		std::printf("%s: %s\n", ok ? "ok" : "FAIL", text);
		if (!ok) ++failures;
	};
	uint32_t values[16] = {100, 200};
	CUtlVector<uint32_t> vec{};
	vec.m_Memory.m_pMemory = values;
	vec.m_Memory.m_nAllocationCount = 16;
	vec.m_Size = 2;
	g_pCUtlMemoryGrow = +[](void* mem, int) -> void* { return mem; };
	std::unordered_set<uint32_t> seeded;
	check(appendToVecLocked(vec, {100, 200}, seeded, "test") == 0 && seeded.empty(),
	      "native package-zero IDs never acquire injected provenance");
	check(appendToVecLocked(vec, {100, 300, 300, 0}, seeded, "test") == 1 &&
	      vec.m_Size == 3 && seeded.size() == 1 && seeded.count(300),
	      "only genuinely missing IDs are appended and marked once");
	check(appendToVecLocked(vec, {100, 300}, seeded, "test") == 0 && vec.m_Size == 3,
	      "a repeated append cannot duplicate IDs");
	vec.m_Size = 2;
	check(appendToVecLocked(vec, {100, 300}, seeded, "test") == 1 && vec.m_Size == 3,
	      "a real package reload can restore previously injected IDs");
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
