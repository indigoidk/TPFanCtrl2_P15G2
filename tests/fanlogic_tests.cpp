// Standalone, hardware-independent unit tests for the pure fan-control logic in
// fanlogic.h. No Win32, no driver, no FANCONTROL object required.
//
// Build & run:  powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
// (compiles with cl.exe; touches nothing in the main project)

#include "../fanlogic.h"
#include <cstdio>

using namespace fanlogic;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond)                                                  \
	do {                                                             \
		++g_checks;                                                  \
		if (!(cond)) {                                               \
			++g_fail;                                                \
			std::printf("FAIL (line %d): %s\n", __LINE__, #cond);    \
		}                                                            \
	} while (0)

// ---- biased_temp -------------------------------------------------------------

static void test_biased_temp() {
	// biasing off -> raw passes through unchanged
	CHECK(biased_temp(60, 5, -1, -1, false) == 60);
	// biasing on, reading ABOVE the disabled window -> offset applies
	CHECK(biased_temp(80, 20, -1, 71, true) == 60);
	// biasing on, reading INSIDE the disabled window -> offset suppressed
	CHECK(biased_temp(50, 20, -1, 71, true) == 50);
	// zero offset is a no-op even when biasing is on
	CHECK(biased_temp(60, 0, 0, 0, true) == 60);
	// negative offset raises the reading (documented behavior)
	CHECK(biased_temp(60, -5, -1, -1, true) == 65);
}

// Default shipped Smart table (hysteresis off): temp, fan, hystUp, hystDown.
static const FanLevel kDefault[] = {
	{ 50, 0, 0, 0 }, { 55, 3, 0, 0 }, { 60, 5, 0, 0 },
	{ 65, 7, 0, 0 }, { 70, 128, 0, 0 }, { -1, 0, 0, 0 }
};
static const int kN = (int)(sizeof(kDefault) / sizeof(kDefault[0]));

// ---- smart_decide ------------------------------------------------------------

static void test_smart() {
	// first decision: 62C -> level idx2 (60->5), no hysteresis on first set
	{
		int last = -1;
		int fan = smart_decide(62, 0, false, 2, kDefault, kN, last);
		CHECK(fan == 5);
		CHECK(last == 2);
	}

	// already at the target level -> no change
	{
		int last = 2;
		int fan = smart_decide(62, 5, false, 2, kDefault, kN, last);
		CHECK(fan == -1);
		CHECK(last == 2);
	}

	// cooled to 52C -> ramp down to idx1 (55->3)
	{
		int last = 2;
		int fan = smart_decide(52, 5, false, 2, kDefault, kN, last);
		CHECK(fan == 3);
		CHECK(last == 1);
	}

	// cooling but inside the down-hysteresis band -> stay (no change, last unchanged)
	{
		const FanLevel t[] = {
			{ 50, 0, 0, 0 }, { 55, 3, 0, 5 }, { 60, 5, 0, 0 },
			{ 65, 7, 0, 0 }, { 70, 128, 0, 0 }, { -1, 0, 0, 0 }
		};
		int last = 2;
		int fan = smart_decide(52, 5, false, 2, t, 6, last);  // 52 > 55-5 -> hold
		CHECK(fan == -1);
		CHECK(last == 2);
	}

	// coming from BIOS (prevMode==1) forces a recompute from zero
	{
		int last = 4;
		int fan = smart_decide(40, 128, false, 1, kDefault, kN, last);
		CHECK(fan == 0);
		CHECK(last == 0);
	}

	// fan currently 64 with Lev64Norm: treated as a normal level (no forced reset),
	// at 40C we ramp down to idx0 (50->0)
	{
		int last = -1;
		int fan = smart_decide(40, 64, true, 2, kDefault, kN, last);
		CHECK(fan == 0);
		CHECK(last == 0);
	}

	// hot start at 90C -> highest level idx4 (70->128)
	{
		int last = -1;
		int fan = smart_decide(90, 0, false, 2, kDefault, kN, last);
		CHECK(fan == 128);
		CHECK(last == 4);
	}
}

// Edge cases tied to the profile-switch fixes: a stale lastLevel (from the
// previous curve) must not be reused, hysteresis must use the supplied table,
// and odd tables must not read out of bounds.
static void test_smart_edges() {
	// A stale lastLevel from a previous profile can wrongly SUPPRESS the first
	// decision after a switch; resetting it to -1 (what ActivateSmartProfile now
	// does) applies the decision immediately. Same inputs, different lastLevel:
	{
		int fresh = -1;
		CHECK(smart_decide(58, 0, false, 2, kDefault, kN, fresh) == 3);   // applied
		CHECK(fresh == 1);

		int stale = 4;   // index that only made sense on the old curve
		CHECK(smart_decide(58, 0, false, 2, kDefault, kN, stale) == -1);  // wrongly held
	}

	// hysteresis uses the *provided* table's hyst fields (a switched-in SM2 curve
	// uses SM2's hysteresis, not a leftover one)
	{
		const FanLevel sm2[] = { { 45, 0, 0, 0 }, { 58, 4, 3, 0 }, { 72, 7, 0, 0 }, { -1, 0, 0, 0 } };
		int last = 0;
		CHECK(smart_decide(59, 0, false, 2, sm2, 4, last) == -1);   // 59 < 58+3 -> hold
		CHECK(last == 0);
		int last2 = 0;
		CHECK(smart_decide(62, 0, false, 2, sm2, 4, last2) == 4);   // 62 >= 61 -> apply
		CHECK(last2 == 1);
	}

	// table with only the terminator -> no decision, no crash
	{
		const FanLevel empty[] = { { -1, 0, 0, 0 } };
		int last = -1;
		CHECK(smart_decide(80, 0, false, 2, empty, 1, last) == -1);
		CHECK(last == -1);
	}

	// table with no -1 terminator must be bounded by n (no out-of-bounds read)
	{
		const FanLevel noterm[] = { { 50, 0, 0, 0 }, { 60, 5, 0, 0 } };
		int last = -1;
		CHECK(smart_decide(65, 0, false, 2, noterm, 2, last) == 5);
		CHECK(last == 1);
	}
}

int main() {
	test_biased_temp();
	test_smart();
	test_smart_edges();

	if (g_fail == 0) {
		std::printf("OK: all %d checks passed.\n", g_checks);
		return 0;
	}
	std::printf("FAILED: %d of %d checks failed.\n", g_fail, g_checks);
	return 1;
}
