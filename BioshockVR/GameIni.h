// BioshockVR/GameIni.h
#pragma once

// Pushes the VR-relevant values from BioshockVR.ini into the GAME's Bioshock.ini
// so the two can never drift apart (a 10-degree FOV drift was the Phase-12 turn
// warp). Call from InitThread, BEFORE the game has read its config where
// possible -- see the note in GameIni.cpp about launch ordering.
//
// Writes only: HorizontalFOV, bHorizontalFOVLock, HorizontalFOVLock,
//              WindowedViewportX/Y.
// Everything else in Bioshock.ini is left strictly alone.
void SyncGameIni();
