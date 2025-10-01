import re
import os
import argparse
from math import isclose

LINE_RE = re.compile(
    r'^(?:(G0?0|G0?1)\s*)?'              # optional G-word
    r'(?=.*[XY])'                        # X or Y exists
    r'(?:X([-+]?\d*\.?\d+)\s*)?'         # optional X
    r'(?:Y([-+]?\d*\.?\d+)\s*)?'         # optional Y
    r'(?:S(\d+))?$'                      # optional S
)

def add_overscan_by_line_grouping(input_path, overscan_distance=2.0, laser_power = 60.0):
    output_lines = []
    lines = []

    with open(input_path, "r") as file:
        lines = file.readlines()
    
    g_val = 0 #set default g_val
    y_val = 0.0 #set default y_val
    x_val = 0.0 #set default y_val
    line_stored = False
    current_line = ""
    current_match = None
    line_stored = False 
    F_set = False
    
    i=0      
    while i < len(lines):    
        if not line_stored:
            line = lines[i].strip() 
            # Match any G0 or G1 line with X, Y, and optional S
            match_start = LINE_RE.match(line)
        else:
            line = current_line
            match_start = current_match
            line_stored = False
        if match_start:
            if match_start.group(1) and (match_start.group(1) == 'G0' or match_start.group(1) == 'G00'):
                g_val = 0
            else: 
                if match_start.group(1) and (match_start.group(1) == 'G1' or match_start.group(1) == 'G01'):
                    g_val = 1
            x_val = float(match_start.group(2)) if match_start.group(2) else x_val
            y_val = float(match_start.group(3)) if match_start.group(3) else y_val
            
            if g_val == 0:
                group_lines = []
                i+=1
                lines_flushed = False
                # Collect all lines before next match
                while i < len(lines):
                    current_line = lines[i].strip()
                    current_match = LINE_RE.match(current_line)
                    if current_match:
                        #Start new sequence if meet G0
                        if current_match.group(1) and (current_match.group(1) == 'G0' or current_match.group(1) == 'G00'):
                            output_lines.append(line)
                            output_lines.extend(group_lines)
                            group_lines = []
                            line_stored = True
                            lines_flushed = True
                            break;
                        else: 
                            if current_match.group(1) and (current_match.group(1) == 'G1' or current_match.group(1) == 'G01'):
                                x_val_last = float(current_match.group(2)) if current_match.group(2) else x_val
                                y_val_last = float(current_match.group(3)) if current_match.group(3) else y_val
                                x_before = x_val
                                x_after = x_val
                                y_before = y_val
                                y_after = y_val
                                if isclose(y_val_last, y_val, abs_tol=1e-6):
                                    if x_val_last > x_val:
                                        x_before = x_val-overscan_distance
                                        x_after = x_val_last+overscan_distance
                                    else:
                                        x_before = x_val+overscan_distance
                                        x_after = x_val_last-overscan_distance
                                else:
                                    if isclose(x_val_last, x_val, abs_tol=1e-6):
                                        if y_val_last > y_val:
                                            y_before = y_val-overscan_distance
                                            y_after = y_val_last+overscan_distance
                                        else:
                                            y_before = y_val+overscan_distance
                                            y_after = y_val_last-overscan_distance
                                output_lines.append(f"G0 X{x_before:.3f} Y{y_before:.3f} S0 (overscan)")
                                output_lines.append(line)
                                output_lines.extend(group_lines)
                                output_lines.append(f"{current_line} S{laser_power:.3f}")
                                output_lines.append(f"G0 X{x_after:.3f} Y{y_after:.3f} S0 (overscan)")
                                group_lines = []
                                lines_flushed = True
                                i+=1
                                x_val = x_after
                                y_val = y_after
                                break
                            else:
                                group_lines.append(current_line)
                                i+=1
                    else:
                        if not re.match(r'^(G0?1)\s+F(\d*\.?\d+)?', current_line):
                            if not re.match(r'^(M03)\s+S(\d*\.?\d+)?', current_line) and not re.match(r'^(M05)',current_line):
                                group_lines.append(current_line)
                        else: 
                            if not F_set:
                                group_lines.append(current_line)
                                group_lines.append("M03 S0")
                                F_set = True
                        i+=1
                if not lines_flushed:
                    output_lines.append(line)
                    output_lines.extend(group_lines)
            else: 
                output_lines.append(line)
                i+=1
        else:
            if not re.match(r'^(G0?1)\s+F(\d*\.?\d+)?', line):
                if not re.match(r'^(M03)\s+S(\d*\.?\d+)?', line) and not re.match(r'^(M05)',line):
                    output_lines.append(line)
            else: 
                if not F_set:
                    output_lines.append(line)
                    output_lines.append("M03 S0")
                    F_set = True
            i+=1
    output_lines.append("M05")
    output_lines.append("G0 X0 Y0 S0")
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
    parser.add_argument("--S", type=float, default=60.0, help="Laser power (default: 60.0)")

    args = parser.parse_args()
    add_overscan_by_line_grouping(args.input_file, overscan_distance=args.overscan, laser_power = args.S)

