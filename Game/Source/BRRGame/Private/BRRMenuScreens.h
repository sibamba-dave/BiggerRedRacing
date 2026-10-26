#pragma once

#include "CoreMinimal.h"

class ABRRFrontEndGameMode;

namespace BRRMenu
{
	// The first screen(s): the main menu, and - back from a race - its results, or the circuit
	// screen the race was started from.
	void Open(ABRRFrontEndGameMode& Menu);
	// A made-up result on RESULSCN, for -BRRMenuWalk (-3).
	void ShowSampleResults(ABRRFrontEndGameMode& Menu);
	// The intro video (-BRRMenuWalk -4) and the victory one (-5).
	void ShowVideo(ABRRFrontEndGameMode& Menu, bool bVictory);
}
