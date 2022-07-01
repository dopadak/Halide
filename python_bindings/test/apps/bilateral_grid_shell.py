"""
Bilateral histogram.
"""

import bilateral_grid
import imageio
import numpy as np
import os
import sys
from timeit import Timer


def main():
    if len(sys.argv) < 5:
        print("Usage: %s input.png output.png range_sigma timing_iterations" % sys.argv[0])
        print("e.g. %s input.png output.png 0.1 10" % sys.argv[0])
        sys.exit(0)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    r_sigma = float(sys.argv[3])
    timing_iterations = int(sys.argv[4])

    assert os.path.exists(input_path), "Could not find %s" % input_path

    input_buf_u8 = imageio.imread(input_path)
    assert input_buf_u8.dtype == np.uint8
    # Convert to float32
    input_buf = input_buf_u8.astype(np.float32)
    input_buf /= 255.0
    output_buf = np.empty(input_buf.shape, dtype=input_buf.dtype)

    t = Timer(lambda: bilateral_grid.bilateral_grid(input_buf, r_sigma, output_buf))
    avg_time_sec = t.timeit(number=timing_iterations) / timing_iterations

    print("Manually-tuned time: %fms" % (avg_time_sec * 1e3))

    # TODO: add autoscheduled time

    output_buf *= 255.0
    output_buf_u8 = output_buf.astype(np.uint8)
    imageio.imsave(output_path, output_buf_u8)

    print("Success!")
    sys.exit(0)


if __name__ == '__main__':
    main()
