// gnubgmodule.cpp - Python 3 extension module for gnubg

#include <Python.h>
#include <dlfcn.h>
#include <libgen.h>
#include <string>
#include <iostream>
#include <fstream>
#include <cstdarg>

#include "stdutil.h"
#include "analyze.h"
#include "bm.h"
#include "player.h"
#include "equities.h"
#include "misc.h"

extern "C" {
    #include "eval.h"
    #include "inputs.h"
    #include "positionid.h"

    typedef struct NetDef NetDef;
    extern NetDef* nets[];    // pulls in the real definition from eval.c :contentReference[oaicite:0]{index=0}:contentReference[oaicite:1]{index=1}
    void initnet(void);       // this will populate nets[] :contentReference[oaicite:2]{index=2}:contentReference[oaicite:3]{index=3}
}

// Hook GNUBG's internal logging function
extern "C" void outputf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

#include "br.h"
#include "osr.h"

// bring in std::vector
#include <vector>

// shorthand 26‐element board for bearoff/resign logic
typedef short int AnalyzeBoard[26];

// The global “analyzer” used for the resign call
namespace {
    Player analyzer;
}

//-----------------------------------------------------------------------------
// Which “ply” to evaluate.  Must match your enum below.
enum {
    PLY_OSR      = -2,
    PLY_BEAROFF  = -3,
    PLY_PRUNE    = -4,
    PLY_1SBEAR   = -5,
    PLY_RACENET  = -6,
    PLY_1ANDHALF = -7,
    PLY_1SRACE   = -8,
};

// ------------------------------------------------------------------
// Given a 10-byte GNUBG “auch” key, emit the 20-char A–Z position ID
static const char*
posString(const unsigned char* auch)
{
    static char buf[21];
    for (int i = 0; i < 10; ++i) {
        buf[2*i]   = 'A' + ((auch[i] >> 4) & 0xF);
        buf[2*i+1] = 'A' +  (auch[i]        & 0xF);
    }
    buf[20] = '\0';
    return buf;
}

//-----------------------------------------------------------------------------
// Convert a 20‐char A–Z key into the 10‐byte “auch” array:
static unsigned char*
auchFromString(const char* str)
{
    // str must be length 20
    static unsigned char auch[10];
    for (int i = 0; i < 10; ++i) {
        int hi = str[2*i]   - 'A';
        int lo = str[2*i+1] - 'A';
        auch[i] = static_cast<unsigned char>((hi<<4) | lo);
    }
    return auch;
}

//─── setBoard for resign logic ─────────────────────────────────────────────────
static void
setBoard(AnalyzeBoard out, const int board[2][25])
{
    // bar of + on [0]
    out[0] = board[1][24];
    // points 1..24
    for (int k = 0; k < 24; ++k) {
        int val = 0;
        if      (board[1][23-k] > 0) { val = board[1][23-k]; }
        else if (board[0][k] > 0)    { val = -board[0][k]; }
        out[k+1] = val;
    }
    // bar of - at index 25
    out[25] = -board[0][24];
}

//─── boardString: turn a C board into the “PositionKey” string ─────────────────
static const char*
boardString(const int board[2][25])
{
    unsigned char auch[10];
    // PositionKey comes from <eval.h>; it wants Board = int[2][25]
    PositionKey((int(*)[25])board, auch);

    // now convert 10 nybbles to 20-char A…Z string
    static char buf[21];
    for (int i = 0; i < 10; ++i) {
        buf[2*i]   = 'A' + ((auch[i] >> 4) & 0xF);
        buf[2*i+1] = 'A' + (auch[i] & 0xF);
    }
    buf[20] = '\0';
    return buf;
}

//-----------------------------------------------------------------------------
// stringToBoard(key, board):
//   key is either a 20-char A–Z string or a 14-char base64 ID;
//   fills int board[2][25] and returns true on success.
static bool
stringToBoard(const char* key, int board[2][25])
{
    size_t len = strlen(key);
    if (len == 20) {
        // interpret as PositionKey
        PositionFromKey(board, auchFromString(key));
        return true;
    }
    else if (len == 14) {
        // interpret as PositionID
        PositionFromID(board, key);
        return true;
    }
    // otherwise invalid
    return false;
}

//-----------------------------------------------------------------------------
// Converter for the “ply” argument: accepts a Python int and checks the range
static int
readPly(PyObject* obj, void* out_p)
{
    int& nPlies = *static_cast<int*>(out_p);
    if (!PyLong_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "ply must be an integer");
        return 0;
    }
    long v = PyLong_AsLong(obj);
    // valid if non-negative or one of our special negative codes
    if (v >= 0 || (v <= PLY_OSR && v >= PLY_1ANDHALF)) {
        nPlies = (int)v;
        return 1;
    }
    PyErr_SetString(PyExc_ValueError, "invalid ply");
    return 0;
}

//-----------------------------------------------------------------------------
// Convert a Python object (26-element sequence or string) into
// a 26-entry AnalyzeBoard (short[26]).  Returns 1 on success, 0 on error.
static int
anyAnalyzeBoard(PyObject* o, void* out_ptr)
{
    // out_ptr really points at an AnalyzeBoard:
    short* board = static_cast<short*>(out_ptr);

    // Case 1: a sequence of length 26
    if (PySequence_Check(o) && PySequence_Size(o) == 26) {
        PyObject* seq = PySequence_Fast(o, "expected 26-element sequence");
        if (!seq) return 0;
        long s0 = 0, s1 = 0;
        for (Py_ssize_t k = 0; k < 26; ++k) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, k);
            if (!PyLong_Check(item)) {
                PyErr_SetString(PyExc_TypeError, "board entries must be ints");
                Py_DECREF(seq);
                return 0;
            }
            long v = PyLong_AsLong(item);
            board[k] = static_cast<short>(v);
            if (v > 0)      s0 += v;
            else if (v < 0) s1 += -v;
        }
        Py_DECREF(seq);
        if (!(s0 <= 15 && s1 <= 15)) {
            PyErr_Format(PyExc_ValueError,
                         "Invalid board (x has %ld, o has %ld)", s0, s1);
            return 0;
        }
        return 1;
    }

    // Case 2: a Unicode string key/ID
    if (PyUnicode_Check(o)) {
        const char* s = PyUnicode_AsUTF8(o);
        if (!s) return 0;  // error already set

        // First decode into the 2×25 int board:
        int temp[2][25];
        if (!stringToBoard(s, temp)) {
            // stringToBoard should set ValueError if invalid
            return 0;
        }
        // Then convert to AnalyzeBoard layout:
        setBoard(board, temp);
        return 1;
    }

    PyErr_SetString(PyExc_ValueError,
                    "Expected 26-element list or position key string");
    return 0;
}

//-----------------------------------------------------------------------------
// pack_board: turn a C int[2][25] into a Python tuple of two 25-tuples
static PyObject*
pack_board(const int board[2][25])
{
    PyObject* outer = PyTuple_New(2);
    if (!outer) return NULL;

    for (int s = 0; s < 2; ++s) {
        PyObject* inner = PyTuple_New(25);
        if (!inner) {
            Py_DECREF(outer);
            return NULL;
        }
        for (int p = 0; p < 25; ++p) {
            PyTuple_SET_ITEM(inner, p,
                             PyLong_FromLong(board[s][p]));
        }
        PyTuple_SET_ITEM(outer, s, inner);
    }
    return outer;
}

//─── anyBoard converter ─────────────────────────────────────────────────────────
static int
anyBoard(PyObject* obj, void* out_board)
{
    // reinterpret the void* as int[2][25]
    int (*board)[25] = reinterpret_cast<int (*)[25]>(out_board);
    if (!PySequence_Check(obj) || PySequence_Size(obj) != 2) {
        PyErr_SetString(PyExc_ValueError, "Expected 2-element sequence of 25-element sequences");
        return 0;
    }
    for (int s = 0; s < 2; ++s) {
        PyObject* row = PySequence_GetItem(obj, s);
        if (!PySequence_Check(row) || PySequence_Size(row) != 25) {
            Py_XDECREF(row);
            PyErr_SetString(PyExc_ValueError, "Each side must be length 25");
            return 0;
        }
        for (int p = 0; p < 25; ++p) {
            PyObject* v = PySequence_GetItem(row, p);
            long x = PyLong_AsLong(v);
            board[s][p] = (int)x;
            Py_DECREF(v);
        }
        Py_DECREF(row);
    }
    return 1;
}

static PyObject* py_boardfromid(PyObject* self, PyObject* args) {
    const char* pos_id;
    if (!PyArg_ParseTuple(args, "s", &pos_id)) {
        return NULL;
    }

    int board[2][25] = {{0}};
    PositionFromID(board, pos_id);

    PyObject* outer = PyList_New(2);
    for (int s = 0; s < 2; ++s) {
        PyObject* inner = PyList_New(25);
        for (int p = 0; p < 25; ++p) {
            PyList_SetItem(inner, p, PyLong_FromLong(board[s][p]));
        }
        PyList_SetItem(outer, s, inner);
    }

    return outer;
}

// Helper: Convert Python list to C int array
bool PyList_ToBoard(PyObject* listObj, int board[2][25]) {
    if (!PyList_Check(listObj) || PyList_Size(listObj) != 2)
        return false;

    for (int s = 0; s < 2; ++s) {
        PyObject* side = PyList_GetItem(listObj, s);
        if (!PyList_Check(side) || PyList_Size(side) != 25)
            return false;

        for (int p = 0; p < 25; ++p) {
            PyObject* val = PyList_GetItem(side, p);
            board[s][p] = PyLong_AsLong(val);
        }
    }
    return true;
}

static PyObject* py_classify(PyObject* self, PyObject* args) {
    PyObject* boardObj;
    if (!PyArg_ParseTuple(args, "O", &boardObj)) return NULL;

    int board[2][25];
    if (!PyList_ToBoard(boardObj, board)) {
        PyErr_SetString(PyExc_ValueError, "Expected 2x25 board list");
        return NULL;
    }

    int cls = ClassifyPosition(board);
    return PyLong_FromLong(cls);
}

static PyObject* py_pubbestmove(PyObject* self, PyObject* args) {
    PyObject* boardObj;
    int dice0, dice1;
    if (!PyArg_ParseTuple(args, "Oii", &boardObj, &dice0, &dice1)) return NULL;

    int board[2][25];
    int move[8] = {0};

    if (!PyList_ToBoard(boardObj, board)) {
        PyErr_SetString(PyExc_ValueError, "Expected 2x25 board list");
        return NULL;
    }

    int n = FindPubevalMove(dice0, dice1, board, move);
    if (n < 0) {
        PyErr_SetString(PyExc_RuntimeError, "No valid move found");
        return NULL;
    }

    PyObject* result = PyList_New(n);
    for (int i = 0; i < n; ++i) {
        PyList_SetItem(result, i, PyLong_FromLong(move[i]));
    }
    return result;
}

static PyObject* py_pubevalscore(PyObject* self, PyObject* args) {
    PyObject* boardObj;
    if (!PyArg_ParseTuple(args, "O", &boardObj)) return NULL;

    int board[2][25];
    if (!PyList_ToBoard(boardObj, board)) {
        PyErr_SetString(PyExc_ValueError, "Expected 2x25 board list");
        return NULL;
    }

    int fRace = ClassifyPosition(board) <= CLASS_RACE;
    float score = pubEvalVal(fRace, board);

    return PyFloat_FromDouble(score);
}

static PyObject* py_id(PyObject* self, PyObject* args) {
    PyObject* boardObj;
    if (!PyArg_ParseTuple(args, "O", &boardObj)) return NULL;

    int board[2][25];
    if (!PyList_ToBoard(boardObj, board)) {
        PyErr_SetString(PyExc_ValueError, "Expected 2x25 board list");
        return NULL;
    }

    const char* id = PositionID(board);
    return PyUnicode_FromString(id);
}

static PyObject* py_rolldice(PyObject* self, PyObject* args) {
    int dice[2];
    RollDice(dice);
    return Py_BuildValue("ii", dice[0], dice[1]);
}

static PyObject*
py_bestmove(PyObject* /*self*/, PyObject* args, PyObject* kwargs)
{
    // args/kwargs locals
    int board_arr[2][25];
    int dice1, dice2;
    int nPlies = 0;
    char side = 0;
    int moveBoard = 0;
    int resignInfo = 0;
    int listMoves = 0;
    int reduced = 0;

    static const char *kwlist[] = {
            "pos","dice1","dice2",
            "n","s","b","r","list","reduced", NULL
    };

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs,
            "O&ii|iciiii",
            const_cast<char**>(kwlist),
            anyBoard, board_arr,
            &dice1, &dice2,
            &nPlies, &side,
            &moveBoard, &resignInfo,
            &listMoves, &reduced
    )) {
        return NULL;
    }

    // Validate inputs
    if (nPlies < 0) {
        PyErr_SetString(PyExc_ValueError, "negative ply");
        return NULL;
    }
    if (dice1 < 0 || dice1 > 6 || dice2 < 0 || dice2 > 6) {
        PyErr_SetString(PyExc_ValueError, "invalid dice");
        return NULL;
    }

    bool xOnPlay;
    switch (side) {
        case 'X': case 'x': xOnPlay = true;  break;
        case 'O': case 'o': xOnPlay = false; break;
        case 0:    xOnPlay = false; break;  // default
        default:
            PyErr_SetString(PyExc_ValueError, "invalid side");
            return NULL;
    }

    std::vector<MoveRecord>* recs = nullptr;
    if (listMoves) recs = new std::vector<MoveRecord>;

    int move_buf[8];
    int n = findBestMove(
            move_buf, dice1, dice2,
            board_arr, xOnPlay, nPlies,
            recs, reduced
    );

    // pack the primary move‐tuple
    PyObject* moves = PyTuple_New(n/2);
    for (int j = 0; j < n/2; ++j) {
        int k = 2*j;
        int from = move_buf[k]>=0 ? move_buf[k]+1 : 0;
        int to   = move_buf[k+1]>=0 ? move_buf[k+1]+1 : 0;
        PyObject* mv2 = PyTuple_New(2);
        PyTuple_SET_ITEM(mv2, 0, PyLong_FromLong(from));
        PyTuple_SET_ITEM(mv2, 1, PyLong_FromLong(to));
        PyTuple_SET_ITEM(moves, j, mv2);
    }

    // if no extra info requested, return just that
    if (!moveBoard && !resignInfo && !listMoves) {
        delete recs;
        return moves;
    }

    // otherwise build the result tuple
    int extra = moveBoard + resignInfo + listMoves;
    PyObject* result = PyTuple_New(1 + extra);
    PyTuple_SET_ITEM(result, 0, moves);

    int idx = 1;

    // 1) new board?
    if (moveBoard) {
        SwapSides(board_arr);
        PyObject* bstr = PyUnicode_FromString(boardString(board_arr));
        PyTuple_SET_ITEM(result, idx++, bstr);
        SwapSides(board_arr);
    }

    // 2) resign info?
    if (resignInfo) {
        int r = 0;
        if (isRace(board_arr)) {
            AnalyzeBoard b;  // your short‐hand 26‐element array
            setBoard(b, board_arr);
            r = analyzer.offerResign(nPlies, 2, b, true);
        }
        PyTuple_SET_ITEM(result, idx++,
                         PyLong_FromLong(r));
    }

    // 3) full move list?
    if (listMoves) {
        PyObject* listT = PyTuple_New(recs->size());
        for (size_t i = 0; i < recs->size(); ++i) {
            const auto& ri = (*recs)[i];
            PyObject* entry = PyTuple_New(4);

            // position key
            PyTuple_SET_ITEM(entry, 0, PyUnicode_FromString(posString(ri.pos)));

            // the individual move steps
            const int *mv = ri.move;
            int cnt = 0;
            while (cnt<4 && mv[2*cnt]>=0) ++cnt;
            PyObject* mvsteps = PyTuple_New(2*cnt);
            for (int j=0; j<cnt; ++j) {
                int f = mv[2*j]>=0 ? mv[2*j]+1 : 0;
                int t = mv[2*j+1]>=0 ? mv[2*j+1]+1 : 0;
                PyTuple_SET_ITEM(mvsteps,2*j,   PyLong_FromLong(f));
                PyTuple_SET_ITEM(mvsteps,2*j+1, PyLong_FromLong(t));
            }
            PyTuple_SET_ITEM(entry,1, mvsteps);

            // probabilities array
            PyObject* probs =
                    PyTuple_New(5);
            for (int k=0; k<5; ++k) {
                PyTuple_SET_ITEM(
                        probs, k,
                        PyFloat_FromDouble(ri.probs[k])
                );
            }
            PyTuple_SET_ITEM(entry,2, probs);

            // match‐score
            PyTuple_SET_ITEM(
                    entry,3,
                    PyFloat_FromDouble(ri.matchScore)
            );

            PyTuple_SET_ITEM(listT,i, entry);
        }
        PyTuple_SET_ITEM(result, idx++, listT);
        delete recs;
    }

    return result;
}

// ------------------------------------------------------------------
// keyofboard(pos: sequence) -> str
static PyObject*
py_keyofboard(PyObject* /*self*/, PyObject* args)
{
    int board_arr[2][25];
    // anyBoard will fill our 2×25 array from a Python sequence
    if (!PyArg_ParseTuple(args, "O&", anyBoard, board_arr)) {
        return NULL;
    }
    // boardString returns the 20-char A–Z key
    const char* key = boardString(board_arr);
    return PyUnicode_FromString(key);
}

//-----------------------------------------------------------------------------
// Python wrapper: boardfromkey(key: str) -> ((25,), (25,))
static PyObject*
py_boardfromkey(PyObject* /*self*/, PyObject* args)
{
    const char* key;
    if (!PyArg_ParseTuple(args, "s", &key))
        return NULL;

    int board[2][25];
    if (!stringToBoard(key, board)) {
        PyErr_SetString(PyExc_ValueError, "invalid board key");
        return NULL;
    }
    return pack_board(board);
}

//-----------------------------------------------------------------------------
// Converter: read either an int or a 6-tuple of bearoff points, producing
// a single bearoff-ID in *pi.
static int
readBearoffId(PyObject* obj, void* pi)
{
    int* out = static_cast<int*>(pi);

    // Case 1: a simple integer
    if (PyLong_Check(obj)) {
        long v = PyLong_AsLong(obj);
        if (!(1 <= v && v < 54264)) {
            PyErr_SetString(
                    PyExc_ValueError,
                    "bearoff id outside of range [1,54264)"
            );
            return 0;
        }
        *out = (int)v;
        return 1;
    }

    // Case 2: a length-6 sequence of pip-counts
    if (PySequence_Check(obj) && PySequence_Size(obj) == 6) {
        PyObject* seq = PySequence_Fast(obj, "expected sequence of length 6");
        if (!seq) return 0;

        int p[6];
        for (int k = 0; k < 6; ++k) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, k);
            long x = PyLong_AsLong(item);
            if (PyErr_Occurred()) {
                Py_DECREF(seq);
                return 0;
            }
            p[k] = (int)x;
        }
        Py_DECREF(seq);

        *out = PositionBearoff(p);
        return 1;
    }

    PyErr_SetString(PyExc_TypeError,
                    "invalid type for bearoff id (int or len-6 sequence required)");
    return 0;
}

static PyObject*
py_bearoffid2pos(PyObject* /*self*/, PyObject* args)
{
    int id;
    // O& with readBearoffId picks up either an integer or a length-6 sequence
    if (!PyArg_ParseTuple(args, "O&", readBearoffId, &id)) {
        return NULL;  // parse error
    }

    int p[6];
    // Fills p[0]..p[5] from the GNUBG bearoff DB
    PositionFromBearoff(p, id);

    // Build a 6-tuple of ints
    return Py_BuildValue(
            "(iiiiii)",
            p[0], p[1], p[2],
            p[3], p[4], p[5]
    );
}

//-----------------------------------------------------------------------------
// Python–3 binding for bearoff probabilities:
//   bearoffprobs(id_or_6tuple) -> tuple of floats
static PyObject*
py_bearoffprobs(PyObject* /*self*/, PyObject* args)
{
    int id;
    // readBearoffId handles either an integer or a 6-element tuple
    if (!PyArg_ParseTuple(args, "O&", readBearoffId, &id)) {
        return NULL;
    }

    // B is GNUBG’s bearoff‐prob struct:
    B b;
    getBearoff(id, &b);

    // number of leading zeros:
    Py_ssize_t zeros = (Py_ssize_t)(b.start - 1);
    Py_ssize_t total = zeros + (Py_ssize_t)b.len;
    PyObject* tup = PyTuple_New(total);
    if (!tup) return NULL;

    // fill leading zeros
    for (Py_ssize_t k = 0; k < zeros; ++k) {
        PyTuple_SET_ITEM(tup, k, PyFloat_FromDouble(0.0));
    }
    // fill the actual probabilities
    for (unsigned int k = 0; k < b.len; ++k) {
        PyTuple_SET_ITEM(
                tup,
                zeros + (Py_ssize_t)k,
                PyFloat_FromDouble(b.p[k])
        );
    }
    return tup;
}

//-----------------------------------------------------------------------------
// moves(board, die1, die2, verbose=0) -> tuple of move-strings or (string,tuple) pairs
static PyObject*
py_moves(PyObject* /*self*/, PyObject* args)
{
    int board_arr[2][25];
    int die1, die2;
    int verbose = 0;

    // parse: O& (anyBoard), ii, optionally one int
    if (!PyArg_ParseTuple(args, "O&ii|i",
                          anyBoard, board_arr,
                          &die1, &die2,
                          &verbose)) {
        return NULL;
    }

    // generate the moves
    movelist ml;
    GenerateMoves(&ml, board_arr, die1, die2);

    // build the result tuple
    PyObject* out = PyTuple_New(ml.cMoves);
    if (!out) return NULL;

    for (unsigned int k = 0; k < (unsigned int)ml.cMoves; ++k) {
        const move& mv = ml.amMoves[k];
        const char* key = posString(mv.auch);

        if (!verbose) {
            // just the 20-char key
            PyTuple_SET_ITEM(
                    out, k,
                    PyUnicode_FromString(key)
            );
        }
        else {
            // build the moves‐tuple for this entry
            unsigned int n = 0;
            while (n < 8 && mv.anMove[n] >= 0) n += 2;

            PyObject* mvSteps = PyTuple_New(n/2);
            for (unsigned int j = 0; j < n/2; ++j) {
                int from = mv.anMove[2*j] >= 0
                           ? mv.anMove[2*j] + 1
                           : 0;
                int to   = mv.anMove[2*j+1] >= 0
                           ? mv.anMove[2*j+1] + 1
                           : 0;
                PyObject* pair = PyTuple_New(2);
                PyTuple_SET_ITEM(pair, 0, PyLong_FromLong(from));
                PyTuple_SET_ITEM(pair, 1, PyLong_FromLong(to));
                PyTuple_SET_ITEM(mvSteps, j, pair);
            }

            // package (key, mvSteps)
            PyObject* entry = PyTuple_New(2);
            PyTuple_SET_ITEM(
                    entry, 0,
                    PyUnicode_FromString(key)
            );
            PyTuple_SET_ITEM(entry, 1, mvSteps);

            PyTuple_SET_ITEM(out, k, entry);
        }
    }

    return out;
}

//-----------------------------------------------------------------------------
// probs(board, nPlies, nr=1296) -> 5‐tuple of floats
static PyObject*
py_probs(PyObject* /*self*/, PyObject* args)
{
    int board_arr[2][25];
    int nPlies;
    unsigned int nr = 1296;

    // note: "O&O&|I"  →  two converter slots, then optional unsigned‐int
    if (!PyArg_ParseTuple(
            args,
            "O&O&|I",
            anyBoard, board_arr,
            readPly,  &nPlies,
            &nr
    )) {
        return NULL;
    }

    float p[5];
    switch(nPlies) {
        case PLY_OSR:
            if (!isRace(board_arr)) {
                PyErr_SetString(PyExc_RuntimeError, "Not a race position");
                return NULL;
            }
            raceProbs(board_arr, p, nr);
            break;
        case PLY_BEAROFF:
            EvaluatePositionToBO(board_arr, p, false);
            break;
        case PLY_PRUNE:
            evalPrune(board_arr, p);
            break;
        case PLY_1SBEAR:
            EvalBearoffOneSided(board_arr, p);
            break;
        case PLY_RACENET:
            NetEvalRace(board_arr, p);
            break;
        case PLY_1ANDHALF: {
            float p1[5];
            EvaluatePosition(board_arr, p, 0,0,0,0,0,0);
            EvaluatePosition(board_arr, p1,1,0,0,0,0,0);
            for (int k=0; k<5; ++k) p[k] = 0.5f*(p[k]+p1[k]);
            break;
        }
        case PLY_1SRACE:
#if defined(OS_BEAROFF_DB)
            if (!EvalOSrace(board_arr, p)) {
            PyErr_SetString(PyExc_RuntimeError,"can't eval OS race DB");
            return NULL;
        }
        break;
#else
            PyErr_SetString(PyExc_RuntimeError,"OS race DB not available");
            return NULL;
#endif
        default:
            EvaluatePosition(board_arr, p, nPlies,0,0,0,0,0);
            break;
    }

    PyObject* out = PyTuple_New(5);
    if (!out) return NULL;
    for (int k = 0; k < 5; ++k) {
        PyTuple_SET_ITEM(out, k, PyFloat_FromDouble(p[k]));
    }
    return out;
}

//-----------------------------------------------------------------------------
// rollout(pos, ngames=1296, n=0, level=Analyze::AUTO, nt=500, std=0)
//    -> (p0,p1,p2,p3,p4)  or  ((p0..p4),(ars0..ars4))
static PyObject*
py_rollout(PyObject* /*self*/, PyObject* args, PyObject* kwargs)
{
    // our 26-element board rep (signed short)
    AnalyzeBoard board;

    // defaults
    int cGames    = 1296;
    int nPlies    = 0;
    Analyze::RolloutEndsAt level = Analyze::AUTO;
    int nTruncate = 500;
    int wantSts   = 0;

    static const char *kwlist[] = {
            "pos", "ngames", "n", "level", "nt", "std", NULL
    };

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs,
            "O&|iiiii",      // one converter + 5 optional ints
            const_cast<char**>(kwlist),
            anyAnalyzeBoard, // fills board[]
            &cGames,
            &nPlies,
            &level,
            &nTruncate,
            &wantSts
    )) {
        return NULL;
    }

    if (cGames <= 0) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid number of games (%d)", cGames);
        return NULL;
    }

    float p[NUM_OUTPUTS];
    float ars[NUM_OUTPUTS];

    // run the rollout
    analyzer.rollout(
            board,
            /*money=*/false,
            p, ars,
            nPlies,
            nTruncate,
            cGames,
            level
    );

    // if std==0, return just the p[5]
    if (!wantSts) {
        return Py_BuildValue(
                "ddddd",
                (double)p[0], (double)p[1], (double)p[2],
                (double)p[3], (double)p[4]
        );
    }

    // otherwise return ((p0..p4),(ars0..ars4))
    return Py_BuildValue(
            "((ddddd)(ddddd))",
            (double)p[0],   (double)p[1],
            (double)p[2],   (double)p[3],
            (double)p[4],

            (double)ars[0], (double)ars[1],
            (double)ars[2], (double)ars[3],
            (double)ars[4]
    );
}

static PyMethodDef GnubgMethods[] = {
        {"classify", py_classify, METH_VARARGS, "Classify a board position."},
//        {"eval_score", py_eval_score, METH_VARARGS, "Evaluate win/gammon/bg probabilities."},
        {"pubbestmove", py_pubbestmove, METH_VARARGS, "Get best move using public evaluation."},
        {"boardfromid", py_boardfromid, METH_VARARGS, "Convert a GNUBG Position ID to a board list"},
        {"boardfromkey",
                (PyCFunction)py_boardfromkey,
                METH_VARARGS,
                "boardfromkey(key: str) -> (two 25-tuples)" },
        { "keyofboard",
                py_keyofboard,
                METH_VARARGS,
                "keyofboard(board) -> position key string" },
        {"id", py_id, METH_VARARGS, "Convert board to Position ID (base64)"},
        {"pubevalscore", py_pubevalscore, METH_VARARGS, "Public evaluation score for the board"},
        {"roll", py_rolldice, METH_VARARGS, "Roll two dice (uses GNUBG RNG)"},
        { "bestmove",
                (PyCFunction)py_bestmove,
                METH_VARARGS|METH_KEYWORDS,
                "bestmove(pos, dice1, dice2, n=0, s=0, b=0, r=0, list=0, reduced=0)\n"
                "Returns either a tuple of moves, or (moves, board?, resign?, list?)."
        },
        { "bearoffid2pos",
                py_bearoffid2pos,
                METH_VARARGS,
                "bearoffid2pos(id) -> tuple of 6 ints giving the bearoff position" },
        { "bearoffprobs",
                py_bearoffprobs,
                METH_VARARGS,
                "bearoffprobs(id_or_6tuple) → tuple of bearoff probabilities" },
        { "moves",
                py_moves,
                METH_VARARGS,
                "moves(board, die1, die2, verbose=0) → tuple of move-keys or (key,steps) pairs" },
        { "probs",
                py_probs,
                METH_VARARGS,
                "probs(board, nPlies, nr=1296)\n\n"
                "Evaluate a position:\n"
                " - board: 2×25 list\n"
                " - nPlies: one of the PLY_* constants\n"
                " - nr: roll count for OSR\n\n"
                "Returns a 5‐tuple of floats (win, gamm, backgammon, etc.)"
        },
        { "rollout",
                (PyCFunction)py_rollout,
                METH_VARARGS | METH_KEYWORDS,
                "rollout(pos, ngames=1296, n=0, level, nt=500, std=0)\n"
                "Perform a rollout:\n"
                "  pos    : 26-element list or key\n"
                "  ngames : number of simulated games\n"
                "  n      : number of plies per rollout\n"
                "  level  : Analyze::AUTO|RACE|BEAROFF|OVER\n"
                "  nt     : truncation length\n"
                "  std    : if nonzero, also return stderr array\n\n"
                "Returns a 5-tuple of floats, or if std=1 a pair of 5-tuples." },
        {NULL, NULL, 0, NULL}
};

static struct PyModuleDef gnubgmodule = {
        PyModuleDef_HEAD_INIT,
        "gnubg",
        "Python bindings for GNUBG neural net evaluation",
        -1,
        GnubgMethods
};

PyMODINIT_FUNC
PyInit_gnubg(void)
{
    Dl_info dl_info;
    dladdr((void*)PyInit_gnubg, &dl_info);
    std::string base = dirname(strdup(dl_info.dli_fname));
    std::string datadir = base + "/gnubg_data";

    std::string weights = datadir + "/gnubg.weights";
    // … any other .bd paths if you need them …

    // 1) Initialize GNUBG (loads all six nets into the global `nets[]`)
    if (!Analyze::init(weights.c_str())) {
        PyErr_SetString(PyExc_ImportError,
                        "Analyze::init() failed to load GNUBG neural nets");
        return NULL;
    }

    // 2) (Optional but strongly recommended) enable SSE optimizations
    useSSE(1);

    // 4) Finally create and return your Python extension
    return PyModule_Create(&gnubgmodule);
}


