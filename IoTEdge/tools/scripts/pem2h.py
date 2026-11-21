"""pem2h.py out.h NAME=file.pem [NAME=file.pem ...] -> C string #defines"""
import sys

BS = "\\"
with open(sys.argv[1], "w", newline="\n") as out:
    for arg in sys.argv[2:]:
        name, fn = arg.split("=", 1)
        lines = open(fn).read().replace("\r", "").strip().split("\n")
        out.write("#define " + name + " " + BS + "\n")
        for l in lines:
            out.write('    "' + l + BS + 'n" ' + BS + "\n")
        out.write('    ""\n\n')
