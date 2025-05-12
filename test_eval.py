import ctypes
from ctypes import POINTER, c_int, c_float, c_char_p, c_void_p, c_uint, c_ubyte, Structure

# Load the GNUBG shared library
lib = ctypes.cdll.LoadLibrary('./libgnubg_nn.so')

# --- Define C Structures ---

class NeuralNet(Structure):
    _fields_ = [
        ('cInput', c_int),
        ('cHidden', c_int),
        ('cOutput', c_int),
        ('rBetaHidden', c_float),
        ('rBetaOutput', c_float),
        ('nTrained', c_int),
        ('arHiddenWeight', c_void_p),
        ('arOutputWeight', c_void_p),
        ('arHiddenThreshold', c_void_p),
        ('arOutputThreshold', c_void_p),
        ('savedBase', c_void_p),
        ('savedIBase', c_void_p),
        ('nEvals', c_int),
    ]

# --- Set Function Signatures ---

# Convert Position ID to board
BoardArrayType = (c_int * 25) * 2
lib.PositionFromID.argtypes = [BoardArrayType, c_char_p]
lib.PositionFromID.restype = None

# Load net
lib.LoadNet.argtypes = [POINTER(NeuralNet), c_char_p, c_int]
lib.LoadNet.restype = c_int

# Destroy net
lib.NeuralNetDestroy.argtypes = [POINTER(NeuralNet)]

# Evaluation function
lib.EvaluatePosition.argtypes = [
    BoardArrayType,          # board
    POINTER(c_float),        # output (win/gammon/bg)
    c_int,                   # nPlies
    c_int,                   # wide
    c_int,                   # direction
    POINTER(c_float),        # p (optional)
    c_uint,                  # snp
    POINTER(c_ubyte)         # pauch (optional)
]
lib.EvaluatePosition.restype = c_int

# --- Step 1: Load the Neural Network ---
nn = NeuralNet()
ret = lib.LoadNet(ctypes.byref(nn), b"nngnubg.weights", 1)  # 1 = contact net
print("LoadNet return:", ret)
assert ret == 0, "Failed to load network"

# --- Step 2: Convert Position ID to board ---
position_id = b"4HPwATDgc/ABMA"  # Opening position
board = BoardArrayType()
lib.PositionFromID(board, position_id)
print("Player 0 board:", list(board[0]))
print("Player 1 board:", list(board[1]))

# Init Eval subsystem
lib.EvalInitialise.restype = c_int
ret = lib.EvalInitialise()
print("EvalInitialise return:", ret)
assert ret == 0, "EvalInitialise failed"

# Prepare output
output_vec = (c_float * 3)()

# Call evaluation
ret = lib.EvaluatePosition(board, output_vec, 1, 0, 0, None, 0, None)
print("EvaluatePosition return:", ret)
print("NN Output [Win %, Gammon %, Backgammon %]:", list(output_vec))

# Cleanup (optional)
lib.NeuralNetDestroy(ctypes.byref(nn))

