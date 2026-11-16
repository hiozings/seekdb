#!/usr/bin/env python3
"""Run with a Python environment containing NumPy, like calibrate.py."""
import unittest
import numpy as np
from calibrate import truth_topk

class CalibrationOracle(unittest.TestCase):
    def test_subtraction_is_widened_before_rounding(self):
        ids=np.array([1,2],dtype=np.int64)
        mask=np.array([True,True])
        for values,query in [([16777216,16777215],-16777216),([3e38,2e38],-3e38)]:
            with self.subTest(values=values):
                matrix=np.array(values,dtype=np.float32).reshape(2,1)
                q=np.array([query],dtype=np.float32)
                self.assertEqual(truth_topk(matrix,ids,q,mask,2).tolist(),[2,1])

if __name__=='__main__': unittest.main()
