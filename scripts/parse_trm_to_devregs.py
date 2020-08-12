#!/usr/bin/python3
"""
Tries to parse pages of Technical Reference Manual to devregs format.
"""
import sys
if sys.version_info < (3, 7):
    sys.exit("local python is too old\n")
import argparse
import logging
import logging.handlers

import popplerqt5
from PyQt5 import QtCore
import re
from dataclasses import dataclass

logger = logging.getLogger('parse_trm')
log_h = logging.StreamHandler()
log_h.setFormatter(logging.Formatter('[%(levelname)s](%(name)s): %(message)s'))
logger.addHandler(log_h)

argp = argparse.ArgumentParser()
argp.add_argument('file', type=argparse.FileType('r'), help="TRM (PDF File)")
argp.add_argument('start', type=int, help="First page in TRM to parse")
argp.add_argument('end', type=int, help="Last page in TRM to parse")
argp.add_argument('-v', '--verbose', action="count", help="loglevel")

r_start = re.compile(r'\d+\.\d+\.\d+\.\d+\s+')
r_name = re.compile(r'^.*?\((?P<rname>\w+)\)', re.M | re.S)
r_adr = re.compile(r"""
        \n\s*Address:.*?=\s(?P<a1>....)_(?P<a2>....)h\n
        """, re.M | re.S | re.X)
r_bitdescription = re.compile(r"""
        ^.*?Reset.*?(0|1).*?Field\s+Description
        """, re.M | re.S | re.X)
r_pagebreak = re.compile(r"""
        (\s{14,}.*\n)?
        \s{20,}Table\scontinues\son\sthe\snext\spage.*\n
        \s{20,}.*\n
        .*NXP\sSemiconductors.*\n
        .*\n
        .*continued.*\n
        .*Field\s+Description
        """,re.X)
r_field = re.compile(r"""
        \n\s{2,12}
        (?P<endbit>\d+)(–(?P<startbit>\d+)|\s).*(?=\n)
        """, re.M | re.X)
r_fldname = re.compile(r"""
        \n\s{0,13}
        (
         ((-)|(Reserved))|
         ((?!\d+)(?!Field)(?P<fldname_multi>\w+_(?=\s.*\n)))|
         ((?!\d+)(?!Field)(?P<fldname_solo>\w+))
        )
        """, re.M | re.X)

def get_nextm(regexp, is_search=True):
    global m
    if is_search:
        tmp = regexp.search(chapter, m.end())
    else:
        tmp = regexp.match(chapter, m.end())
    if tmp:
        m = tmp
        return True
    else:
        return False

def get_fldname(fieldname=str("")):
    if get_nextm(r_pagebreak, is_search=False):
        logger.debug("\t\tpagebreak")
        #logger.debug("matches: " + chapter[m.start():m.end()])
    ret = get_nextm(r_fldname)
    if not ret == True:
        raise RuntimeError("cannot find fldname")
    if m.group("fldname_solo"):
        fieldname += m.group("fldname_solo")
        logger.debug("\t\tfieldname_solo: " + fieldname)
        return fieldname
    elif m.group("fldname_multi"):
        fieldname += m.group("fldname_multi")
        logger.debug("\t\tfieldname_multi: " + fieldname)
        return get_fldname(fieldname)
    else:
        logger.debug("\t\tfield: " + "--Reserved--")
    return False

if __name__ == '__main__':
    args = argp.parse_args()

    if args.verbose:
        logger.setLevel(logging.DEBUG)

    d = popplerqt5.Poppler.Document.load(args.file.name)
    txt = ""
    for i in range(args.start - 1, args.end):
        txt = txt + d.page(i).text(QtCore.QRectF())

    logger.debug("text from pdf: " + txt)

    # search n split chapters
    chapters = r_start.split(txt)
    chapters.pop(0) #drop garbage before first chapter

    for chapter in chapters:
        logger.debug("->chapter: " + chapter)

        # search register name
        m = r_name.match(chapter)
        if m == None:
            exit()
        regname = str(m.group("rname"))
        logger.debug("\tregister name: " + regname)

        # search register address
        if not get_nextm(r_adr):
            address = "FIXME"   # can be defined like: "where i=0d to 3d"
            #raise RuntimeError("no address found")
        else:
            a1 = m.group("a1")
            a2 = m.group("a2")
            address = "0x" + str(a1) + str(a2)
        logger.debug("\taddress: " + address)
        logger.debug("")

        space = "\t"
        if len(regname) < 16:
            space += "\t"
        if len(regname) < 8:
            space += "\t"

        print(regname + space + address)

        # search bitdescription
        if not get_nextm(r_bitdescription):
            raise RuntimeError("no bitdescription found")

        # search bit number of field
        eb = None
        sb = None
        field = None
        while get_nextm(r_field):
            eb = m.group("endbit")
            sb = m.group("startbit")
            if sb is not None:
                field = eb + "-" + sb
            else:
                field = eb

            logger.debug("\t\tfield: " + field)

            # search its corresponding name
            fieldname = get_fldname()
            if fieldname:
                print("\t:" + fieldname + ":" + field)

            logger.debug("")

        # last name of bitfield can lack number of field
        if field is not None and "-0" in field or "0" is field:
            continue
        ending_fieldname = get_fldname()
        if not ending_fieldname:
            continue

        if sb is not None:
            field = str(int(sb) - 1) + "-" + "0"
        elif eb is not None:
            field = str(int(eb) - 1) + "-" + "0"
        elif sb is None and eb is None:
            field = "31" + "-" + "0"
        else:
            field = "0"

        if field:
            print("\t:" + ending_fieldname + ":" + field)

