// Shared by our plugins (the master: Tools\Shared Plugin Code\NoteRingChecks.h): NoteRing's off-game checks
// GPL-3.0-or-later; see each plugin's LICENSE.txt.

#pragma once

#include "NoteRing.h"

#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace NoteRingChecks
{
	using Check = void (*)(bool a_ok, const char* a_what, int a_line);

	inline constexpr int kCount = 12;

#define NOTERING_CHECK(x) a_check((x), #x, __LINE__)

	inline void Run(Check a_check)
	{
		// nothing noted: the drain reads no slot and reports no loss
		{
			NoteRing<8> ring;
			int         taken = 0;
			NOTERING_CHECK(!ring.Armed());
			NOTERING_CHECK(!ring.Drain([&](std::uint64_t) { ++taken; }) && taken == 0);
		}
		// notes come out once each; a second drain finds nothing; the empty note is never written
		{
			NoteRing<8>             ring;
			std::set<std::uint64_t> got;
			ring.Note(0);
			NOTERING_CHECK(!ring.Armed());
			ring.Note(5);
			ring.Note(7);
			NOTERING_CHECK(ring.Armed());
			const bool                    lost = ring.Drain([&](std::uint64_t a_note) { got.insert(a_note); });
			const std::set<std::uint64_t> want{ 5, 7 };
			NOTERING_CHECK(!lost && got == want);
			int again = 0;
			NOTERING_CHECK(!ring.Drain([&](std::uint64_t) { ++again; }) && again == 0 && !ring.Armed());
		}
		// a full ring takes every note; one more than it holds reports the loss once, and the next drain is clean
		{
			NoteRing<4> ring;
			int         taken = 0;
			for (std::uint64_t i = 1; i <= 4; ++i) {
				ring.Note(i);
			}
			NOTERING_CHECK(!ring.Drain([&](std::uint64_t) { ++taken; }) && taken == 4);
			taken = 0;
			for (std::uint64_t i = 1; i <= 5; ++i) {
				ring.Note(i);
			}
			NOTERING_CHECK(ring.Drain([&](std::uint64_t) { ++taken; }) && taken == 4);
			ring.Note(9);
			taken = 0;
			NOTERING_CHECK(!ring.Drain([&](std::uint64_t) { ++taken; }) && taken == 1);
		}
		// four threads note at once, the reader drains beside them: every note is taken once, or its loss is reported
		{
			NoteRing<1024>           ring;
			std::vector<std::thread> writers;
			for (std::uint64_t t = 0; t < 4; ++t) {
				writers.emplace_back([&ring, t]() {
					for (std::uint64_t i = 1; i <= 200; ++i) {
						ring.Note(t * 1000 + i);
					}
				});
			}
			std::multiset<std::uint64_t> got;
			bool                         lost = false;
			for (int pass = 0; pass < 50; ++pass) {
				lost = ring.Drain([&](std::uint64_t a_note) { got.insert(a_note); }) || lost;
			}
			for (auto& w : writers) {
				w.join();
			}
			lost = ring.Drain([&](std::uint64_t a_note) { got.insert(a_note); }) || lost;
			bool once = true;
			for (const auto note : got) {
				once = once && got.count(note) == 1;
			}
			NOTERING_CHECK(once);
			NOTERING_CHECK(lost || got.size() == 800);
			NOTERING_CHECK(!lost);
		}
	}

#undef NOTERING_CHECK
}
