// gnubg_api.cc
#include "eval.h"
#include <ctime>

// 🔧 Force C linkage for FindBestMove
extern "C" int FindBestMove(unsigned int, int*, int, int, int[2][25], unsigned int, int);

extern "C" {

// Initialize GNUBG: weights and bearoff databases
const char* EvalInitialiseWrapper(const char* weights, const char* os_db, const char* ts_db, const char* bearoff_db) {
    return EvalInitialise(weights, os_db, ts_db, bearoff_db);
}

// Enable shortcut evaluation
int SetShortCutsWrapper(int use) {
    return setShortCuts(use);
}

// Configure net evaluation shortcut space
void SetNetShortCutsWrapper(unsigned int nMoves) {
    setNetShortCuts(nMoves);
}

// Seed GNUBG’s internal RNG
void SeedRandom(unsigned int seed) {
    srandom(seed);
}

// Wrapper for FindBestMove
int FindBestMoveWrapper(
        int nPlies,
        int anMove[8],
        int dice1,
        int dice2,
        const int anBoard[2][25],
        int direction
) {
    return FindBestMove(nPlies, anMove, dice1, dice2, const_cast<int(*)[25]>(anBoard), 0, direction);
}

}
extern "C" {

// Simplified: Analyze moves and return equity list
int EvaluateMoves(
        const int board[2][25],
        int dice1,
        int dice2,
        int nPlies,
        int direction,
        int* outMoves,       // 32 x 8 ints = 256 ints
        float* outEquities,  // 32 floats
        int* outCount        // 1 int
) {
    movelist ml;
    move* moves;
    float scores[32][NUM_OUTPUTS];

    int result = GenerateMoves(&ml, board, dice1, dice2);
    if (result <= 0) {
        *outCount = 0;
        return -1;
    }

    FindBestMoves(&ml, scores, nPlies, dice1, dice2, board, direction, 0.001f);

    moves = ml.amMoves;
    *outCount = ml.cMoves;

    for (int i = 0; i < ml.cMoves && i < 32; ++i) {
        for (int j = 0; j < 8; ++j)
            outMoves[i * 8 + j] = moves[i].anMove[j];
        outEquities[i] = scores[i][0];  // OUTPUT_WIN
    }

    return 0;
}
}
