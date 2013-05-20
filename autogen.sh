#!/bin/sh
# Run this to generate all the initial makefiles, etc.

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

PKG_NAME="Empathy"
REQUIRED_AUTOMAKE_VERSION=1.9

(test -f $srcdir/configure.ac) || {
    echo -n "**Error**: Directory "\`$srcdir\'" does not look like the"
    echo " top-level gnome directory"
    exit 1
}

which gnome-autogen.sh || {
    echo "You need to install gnome-common from the GNOME GIT"
    exit 1
}

# Fetch submodules if needed
if test ! -f libempathy-gtk/egg-list-box/COPYING -o ! -f telepathy-account-widgets/COPYING;
then
  echo "+ Setting up submodules"
  git submodule init
fi
git submodule update

cd libempathy-gtk/egg-list-box
sh autogen.sh --no-configure
cd ../..

cd telepathy-account-widgets
sh autogen.sh --no-configure
cd ..

USE_GNOME2_MACROS=1 USE_COMMON_DOC_BUILD=yes . gnome-autogen.sh
