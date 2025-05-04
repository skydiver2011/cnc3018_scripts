import re
import os
import argparse
from math import isclose

def add_overscan_by_line_grouping(input_path, overscan_distance=2.0):
    output_lines = []
    lines = []

    with open(input_path, "r") as file:
        lines = file.readlines()

    y_val = 0.0 #set default y_val
    line_stored = False
    current_line = ""
    current_match = None

    i = 0
    while i < len(lines):
        if not line_stored:
            line = lines[i].strip()
            # Match any G0 or G1 line with X, Y, and optional S
            match_start = re.match(r'^(G[01])\s+X([-+]?\d*\.?\d+)(?:\s+Y([-+]?\d*\.?\d+))?(?:\s+S(\d+))?', line)
        else:
            line = current_line
            match_start = current_match
        if match_start:
            y_val = float(match_start.group(3)) if match_start.group(3) else y_val
            group_lines = []
            x_vals = []
            
            # Add first line to the group
            x_vals.append(float(match_start.group(2)))
            group_lines.append(line)
            i+=1

            # Collect all lines that match this same Y
            while i < len(lines):
                current_line = lines[i].strip()
                current_match = re.match(r'^(G[01])\s+X([-+]?\d*\.?\d+)(?:\s+Y([-+]?\d*\.?\d+))?(?:\s+S(\d+))?', current_line)
                if current_match and ((current_match.group(3) and isclose(float(current_match.group(3)), y_val, abs_tol=1e-6)) or not current_match.group(3)):
                    x_vals.append(float(current_match.group(2)))
                    group_lines.append(current_line)
                    i += 1
                else:
                    break

            # Compute overscan start/end
            x_min = min(x_vals)
            x_max = max(x_vals)
            x_before = x_min - overscan_distance
            x_after = x_max + overscan_distance
            
            if len(x_vals) <2 :
                # Add original lines
                output_lines.extend(group_lines)
            else:
                if x_vals[0] < x_vals[1]:
                    # Add overscan start G0
                    output_lines.append(f"G0 X{x_before:.3f} Y{y_val:.3f} S0 (overscan)")

                    # Add original lines
                    output_lines.extend(group_lines)

                    # Add overscan end G0
                    output_lines.append(f"G0 X{x_after:.3f} Y{y_val:.3f} S0 (overscan)")
                else:
                    # Add overscan end G0
                    output_lines.append(f"G0 X{x_after:.3f} Y{y_val:.3f} S0 (overscan)")
                    
                    # Add original lines
                    output_lines.extend(group_lines)

                    # Add overscan start G0
                    output_lines.append(f"G0 X{x_before:.3f} Y{y_val:.3f} S0 (overscan)")
                    
            line_stored = True
        else:
            output_lines.append(line)
            line_stored = False
            i += 1

    # Save to new file
    base, ext = os.path.splitext(input_path)
    output_file = f"{base}_overscan{ext}"
    with open(output_file, "w") as file:
        file.write('\n'.join(output_lines))

    return output_file
    
# ----------------------
# CLI Entry Point
# ----------------------
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Add horizontal overscan to LaserGRBL-style G-code.")
    parser.add_argument("input_file", help="Input .gcode or .nc file")
    parser.add_argument("--overscan", type=float, default=2.0, help="Overscan distance in mm (default: 2.0)")

    args = parser.parse_args()
    add_overscan_by_line_grouping(args.input_file, overscan_distance=args.overscan)

