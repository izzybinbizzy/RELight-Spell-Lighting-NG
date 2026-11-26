// Shared by our plugins (the master: Tools\Shared Plugin Code\NoteRing.h)
// GPL-3.0-or-later; see each plugin's LICENSE.txt.
//
// A note from the game's loader or job threads to the main thread with no lock and no allocation (the plugin framework, section 7a,
// Threads): a fixed ring of non-zero 64-bit notes, written by any thread, read by ONE thread. A note that lands on a slot not yet
// read sets the overflow word, so the reader knows it lost some and can look round once instead. Game-free, tested off the game.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

template <std::size_t N>
class NoteRing
{
	static_assert(N > 0 && (N & (N - 1)) == 0, "the ring's size is a power of two");

public:
	// any thread; a_note 0 is the empty slot and is never written
	void Note(std::uint64_t a_note) noexcept
	{
		if (a_note == 0) {
			return;
		}
		const auto at = _head.fetch_add(1, std::memory_order_relaxed) & (N - 1);
		if (_slots[at].exchange(a_note, std::memory_order_acq_rel) != 0) {
			_overflow.store(true, std::memory_order_relaxed);
		}
		_armed.store(true, std::memory_order_release);
	}

	// the one reader: each note at most once, then whether any was lost since the last drain. Nothing armed = no slot is read.
	// A note written while the drain runs is taken now or by the next drain
	template <class F>
	bool Drain(F&& a_take)
	{
		if (!_armed.exchange(false, std::memory_order_acq_rel)) {
			return false;
		}
		for (auto& slot : _slots) {
			if (const auto note = slot.exchange(0, std::memory_order_acq_rel); note != 0) {
				a_take(note);
			}
		}
		return _overflow.exchange(false, std::memory_order_relaxed);
	}

	[[nodiscard]] bool Armed() const noexcept { return _armed.load(std::memory_order_relaxed); }

private:
	std::array<std::atomic<std::uint64_t>, N> _slots{};
	std::atomic<std::uint32_t>                _head{ 0 };
	std::atomic<bool>                         _overflow{ false };
	std::atomic<bool>                         _armed{ false };
};
