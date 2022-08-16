#!/usr/bin/python3
# svd to devregs dat file: use svd file to generate devregs.dat file.
# devregs is a linux userspace application allow to read/write a register
# so useful for debuging. This script generate dat file to simplify usage.
#
# usage: python ./svd2devregs.py -s <SVD file> -d <DAT file>
#
# Authors:
#  Ludovic Barre <ludovic.barre@st.com>
#
# This work is licensed under the terms of the GNU GPL version 2.
#
import os
import argparse
from cmsis_svd.parser import SVDParser

def gen_svd2dat(svd_file, dat_file):
    """Use svd parser to generate a dat file"""

    try:
        parser = SVDParser.for_xml_file(svd_file)
    except:
        print("Error to parse svd file:{}".format(svd_file))

    try:
        with open(dat_file, "w") as fileout:
            for peripheral in parser.get_device().peripherals:
                fileout.write("{}_BASE\t\t{:#010x}\n".format(peripheral.name, peripheral.base_address))
                for register in peripheral.registers:
                    fileout.write("{}_{}\t\t{:#010x}\n".format(peripheral.name, register.name, peripheral.base_address + register.address_offset))
                    for field in register.fields:
                        if field.bit_width > 1:
                            fileout.write("\t:{}_{}:{}-{}\n".format(peripheral.name, field.name, field.bit_offset, field.bit_offset + field.bit_width -1))
                        else:
                            fileout.write("\t:{}_{}:{}\n".format(peripheral.name, field.name, field.bit_offset))

    except Exception as inst:
        print("Error writting {}: {}\n".format(dat_file, inst))
    else:
        print("SVD File {} generated".format(dat_file))


def main():
    parser = argparse.ArgumentParser(description="parse svd file to generate .dat file ")
    # Add the arguments
    parser.add_argument('-s', '--svd_file', help='svd file', required=True)
    parser.add_argument('-d', '--dat_file', help='dat file', required=True)
    
    args = parser.parse_args()

    if os.path.isfile(args.svd_file) is False:
        raise Exception("No such file:{}".format(args.svd_file))

    gen_svd2dat(args.svd_file, args.dat_file)

if __name__ == "__main__":
    main()
