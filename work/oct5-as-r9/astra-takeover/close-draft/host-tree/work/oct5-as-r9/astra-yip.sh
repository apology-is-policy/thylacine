#!/bin/sh
# Operator transferred this checkout to Astra on October 8. Keep Yip identity
# rooted in Astra's permanent checkout; never pass --as or use Corona's line.
set -eu
cd /Users/northkillpd/projects/thylacine-astra
exec yip "$@"
