#!/bin/sh

export LANG=C
OS=`uname -s`
if [ "$OS" = "FreeBSD" ]
then
    MAKE=gmake
else
    MAKE=make
fi

$MAKE >./make1.txt 2>./make2.txt
