import ctypes
from ctypes import *
import numpy as np
import os
import time

# Load the compiled shared library
lib = ctypes.CDLL('./libgnubg_nn.so')

# --- Bindings to C functions ---

# Initialization
lib.EvalInitialiseWrapper.argtypes = [c_char_p, c_char_p, c_char_p, c_char_p]
lib.EvalInitialiseWrapper.restype = c_char_p

lib.SetShortCutsWrapper.argtypes = [c_int]
lib.SetShortCutsWrapper.restype = c_int

lib.SetNetShortCutsWrapper.argtypes = [c_uint]
lib.SetNetShortCutsWrapper.restype = None

lib.SeedRandom.argtypes = [c_uint]
lib.SeedRandom.restype = None

# Find best move
lib.FindBestMoveWrapper.argtypes = [
    c_int,                     # nPlies
    POINTER(c_int * 8),        # anMove[8]
    c_int, c_int,              # dice1, dice2
    POINTER((c_int * 25) * 2), # board[2][25]
    c_int                      # direction
]
lib.FindBestMoveWrapper.restype = c_int

lib.EvaluateMoves.argtypes = [
    POINTER((c_int * 25) * 2),  # board[2][25]
    c_int, c_int,               # dice1, dice2
    c_int,                      # nPlies
    c_int,                      # direction
    POINTER(c_int * 256),       # 32 x 8 moves
    POINTER(c_float * 32),      # equities
    POINTER(c_int)              # move count
]
lib.EvaluateMoves.restype = c_int

# --- Helper Functions ---

def initialise_gnubg():
    weights = b"./gnubg.weights"
    os_db   = b"./gnubg_os0.bd"
    ts_db   = b"./gnubg_ts0.bd"
    os_bdb  = b"./gnubg_os.db"

    version = lib.EvalInitialiseWrapper(weights, os_db, ts_db, os_bdb)
    if not version:
        raise RuntimeError("❌ EvalInitialiseWrapper failed")

    print(f"✅ GNUBG initialized (version: {version.decode()})")
    lib.SetShortCutsWrapper(1)
    lib.SetNetShortCutsWrapper(32)
    lib.SeedRandom(int(time.time()))

def create_starting_board():
    board = np.zeros((2, 25), dtype=np.int32)
    board[0][0] = 2
    board[0][11] = 5
    board[0][16] = 3
    board[0][18] = 5
    board[1][23] = 2
    board[1][12] = 5
    board[1][7] = 3
    board[1][5] = 5
    return board

def call_find_best_move(board: np.ndarray, dice: tuple[int, int], nPlies=0, direction=0):
    assert board.shape == (2, 25), "Board must be shape (2, 25)"

    rows = [(c_int * 25)(*row) for row in board]
    board_c = ((c_int * 25) * 2)(*rows)

    move_result = (c_int * 8)()
    rc = lib.FindBestMoveWrapper(
        nPlies,
        byref(move_result),
        dice[0],
        dice[1],
        byref(board_c),
        direction
    )

    return rc, list(move_result)

def evaluate_moves(board: np.ndarray, dice: tuple[int, int], nPlies=2, direction=0):
    rows = [(c_int * 25)(*row) for row in board]
    board_c = ((c_int * 25) * 2)(*rows)

    moves = (c_int * (32 * 8))()
    equities = (c_float * 32)()
    count = c_int(0)

    rc = lib.EvaluateMoves(
        byref(board_c),
        dice[0],
        dice[1],
        nPlies,
        direction,
        byref(moves),
        byref(equities),
        byref(count)
    )

    if rc != 0 or count.value == 0:
        raise RuntimeError("Evaluation failed")

    results = []
    for i in range(count.value):
        move = [moves[i * 8 + j] for j in range(8)]
        equity = equities[i]
        results.append((move, equity))

    return results


# --- Main ---

if __name__ == "__main__":
    initialise_gnubg()
    board = create_starting_board()
    dice = (3, 5)
    rc, move = call_find_best_move(board, dice)
    print(f"RC: {rc}")
    print("Best move:", move)

    board = create_starting_board()
    dice = (3, 5)
    results = evaluate_moves(board, dice)

    for i, (move, eq) in enumerate(results):
        print(f"{i+1}. Move: {move} | Equity: {eq:.4f}")

