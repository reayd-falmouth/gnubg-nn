# -*- coding: utf-8 -*-
#!/usr/bin/env python

import gnubg

# Load the board using the Position ID
board = gnubg.boardfromkey("4HPwATDgc/ABMA")

# Evaluate that board using the pub-style neural net
result = gnubg.pubevalscore(board)

# Print raw output
print("GNUBG Neural Net Output:")
for key in ['win', 'wing', 'winbg', 'loseg', 'losebg']:
    print("%-15s: %.6f" % (key, result[key]))

# GNUBG-style equity formula
def gnubg_equity(r):
    win = r['win']
    wing = r['wing']
    winbg = r['winbg']
    loseg = r['loseg']
    losebg = r['losebg']
    return 2 * win + wing + 1.5 * winbg - 1 - loseg - 1.5 * losebg

print("\nGNUBG-style Equity: %.6f" % gnubg_equity(result))
