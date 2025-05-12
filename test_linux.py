import ctypes
from ctypes import POINTER, c_int, c_float, c_char_p, Structure
import os
from ctypes import cast, POINTER, c_float, c_void_p
for f in ["nngnubg.weights", "gnubg_os0.bd", "gnubg_ts0.bd"]:
    assert os.path.exists(f), f"Missing: {f}"


# Load the library
lib = ctypes.cdll.LoadLibrary('./libgnubg_nn.so')

# Board type: int[2][25]
BoardArrayType = (c_int * 25) * 2

# Set function signatures
lib.PositionFromID.argtypes = [BoardArrayType, c_char_p]
lib.PositionFromID.restype = None

lib.EvalInitialise.argtypes = [c_char_p, c_char_p, c_char_p]
lib.EvalInitialise.restype = c_char_p  # returns version string

lib.setNets.argtypes = []
lib.setNets.restype = None

lib.EvaluatePosition.argtypes = [
    BoardArrayType,
    POINTER(c_float),  # arOutput
    c_int,  # nPlies
    c_int,  # wide
    c_int,  # direction
    POINTER(c_float),  # p (optional rollout buf)
    c_int,  # snp
    ctypes.c_void_p  # pauch (optional)
]
lib.EvaluatePosition.restype = c_int

lib.EvalShutdown.argtypes = []
lib.EvalShutdown.restype = None

# Init neural net (just load the weights, no bearoff DBs)
weights_file = b"nngnubg.weights"
os_db = b"gnubg_os0.bd"
ts_db = b"gnubg_ts0.bd"

version = lib.EvalInitialise(weights_file, os_db, ts_db)


if not version:
    raise RuntimeError("Failed to initialize neural network weights.")
print("EvalInitialise returned version:", ctypes.cast(version, c_char_p).value.decode())

lib.setNets()  # Required after EvalInitialise

# # Convert position from GNUBG Position ID to board array
board = BoardArrayType()
position_id = b"4HPwATDgc/ABMA"
lib.PositionFromID(board, position_id)
#
# # Evaluate position
output = (c_float * 5)()  # 5 = WIN, WINGAMMON, WINBG, LOSEGAMMON, LOSEBG

ret = lib.EvaluatePosition(
    board,
    output,
    2,        # nPlies
    0,        # wide
    0,        # direction (usually 1 means X to play)
    cast(None, POINTER(c_float)),  # 🧠 Explicit NULL for p
    0,                              # snp
    cast(None, c_void_p)            # 🧠 Explicit NULL for pauch
)

# print("EvaluatePosition return:", ret)
# print("NN Output [Win %, Gammon %, Backgammon %]:", list(output))
#
# # Clean shutdown
# lib.EvalShutdown()
