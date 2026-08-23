#include "../src/feats/libraryremoval_policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}
}

int main()
{
	// FillInAppOverview reads the field through whichever register the compiler
	// parked the app pointer in: `mov eax,[ecx+disp8]` on the 2026-08-03 client
	// and `mov eax,[edx+disp8]` on 2026-08-16. Both are the same read, so both
	// modrm forms must decode; anything else must still fail closed.
	constexpr std::array<std::uint8_t, 3> ownershipReadEcx{0x8B, 0x41, 0x18};
	constexpr std::array<std::uint8_t, 3> ownershipReadEdx{0x8B, 0x42, 0x18};
	constexpr std::array<std::uint8_t, 3> wrongOpcode{0x89, 0x41, 0x18};
	constexpr std::array<std::uint8_t, 3> wrongDestination{0x8B, 0x49, 0x18};
	constexpr std::array<std::uint8_t, 3> indirectBase{0x8B, 0x44, 0x18};
	constexpr std::array<std::uint8_t, 3> unalignedOffset{0x8B, 0x41, 0x19};
	check(LibraryRemovalPolicy::deriveOwnershipOffset(ownershipReadEcx) == 0x18,
	      "validated Linux instruction derives the ownership offset");
	check(LibraryRemovalPolicy::deriveOwnershipOffset(ownershipReadEdx) == 0x18,
	      "the same read through edx derives the same ownership offset");
	check(!LibraryRemovalPolicy::deriveOwnershipOffset(wrongOpcode),
	      "wrong ownership-read opcode is rejected");
	check(!LibraryRemovalPolicy::deriveOwnershipOffset(wrongDestination),
	      "a read into another register is rejected");
	check(!LibraryRemovalPolicy::deriveOwnershipOffset(indirectBase),
	      "a SIB-addressed read is rejected");
	check(!LibraryRemovalPolicy::deriveOwnershipOffset(unalignedOffset),
	      "unaligned ownership field is rejected");
	check(LibraryRemovalPolicy::hiddenOwnershipFlags(0x0803) == 0,
	      "visual removal clears every ownership presentation flag");

	LibraryRemovalPolicy::Queue queue(4);
	check(queue.push(10), "first removal queues");
	check(!queue.push(10), "duplicate removal coalesces");
	queue.cancel(10);
	check(!queue.drainOne().has_value(), "re-add cancels pending removal");

	check(queue.push(10) && queue.push(20), "distinct removals queue");
	const auto remove10 = queue.drainOne();
	check(remove10 && remove10->appId == 10 &&
	      remove10->action == LibraryRemovalPolicy::Action::Remove,
	      "one removal drains per frame");
	check(queue.begin(*remove10), "current removal begins");
	queue.finish(*remove10);
	const auto remove20 = queue.drainOne();
	check(remove20 && remove20->appId == 20 &&
	      remove20->action == LibraryRemovalPolicy::Action::Remove,
	      "next removal drains on the next frame");
	check(queue.begin(*remove20), "second removal begins");
	queue.finish(*remove20);

	queue.cancel(10);
	check(queue.appliedSnapshot() == std::vector<std::uint32_t>({20}),
	      "full snapshots retain only removals that are still desired");
	queue.requestFullReassert();
	const auto reassert20 = queue.drainOne();
	check(reassert20 && reassert20->appId == 20 &&
	      reassert20->action == LibraryRemovalPolicy::Action::Remove,
	      "full rebuild reasserts only ids that remain removed");
	check(queue.begin(*reassert20), "reasserted removal begins");
	queue.finish(*reassert20);

	check(queue.push(99), "a new removal can enter the race fixture");
	const auto racingRemove = queue.drainOne();
	check(racingRemove && queue.begin(*racingRemove),
	      "removal can enter the Steam mutation window");
	queue.cancel(99);
	queue.readyToRestore(99);
	queue.finish(*racingRemove);
	const auto restore10 = queue.drainOne();
	check(restore10 && restore10->appId == 99 &&
	      restore10->action == LibraryRemovalPolicy::Action::Restore,
	      "a re-add racing an in-flight removal queues compensation");
	check(queue.begin(*restore10), "current restoration begins");
	queue.finish(*restore10);
	check(queue.appliedSnapshot() == std::vector<std::uint32_t>({20}),
	      "compensation restores the authoritative present state");

	check(queue.push(98), "inverse race fixture queues removal");
	const auto first98 = queue.drainOne();
	check(first98 && queue.begin(*first98), "inverse fixture removal begins");
	queue.finish(*first98);
	queue.cancel(98);
	queue.readyToRestore(98);
	const auto restore98 = queue.drainOne();
	check(restore98 && queue.begin(*restore98),
	      "inverse fixture restoration enters the mutation window");
	check(queue.push(98), "a newer removal supersedes in-flight restoration");
	queue.finish(*restore98);
	const auto final98 = queue.drainOne();
	check(final98 && final98->appId == 98 &&
	      final98->action == LibraryRemovalPolicy::Action::Remove,
	      "latest desired removed state receives compensating work");
	check(queue.begin(*final98), "compensating removal begins");
	queue.finish(*final98);
	queue.cancel(98);

	check(queue.push(30), "new removal queues after applied entries");
	check(queue.push(40), "capacity admits the complete desired set");
	check(queue.push(50), "capacity counts desired removals, not stale nodes");
	check(!queue.push(60), "desired removal set is bounded");
	queue.cancel(30);
	check(queue.push(60), "cancelling a removal releases bounded capacity");
	queue.close();
	check(!queue.push(70) && !queue.drainOne().has_value(),
	      "closed queue refuses work during hook teardown");
	queue.reopen();
	check(queue.push(70) && queue.drainOne()->appId == 70,
	      "a later hook setup starts with a fresh queue");

	return failures == 0 ? 0 : 1;
}
