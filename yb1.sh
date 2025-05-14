#!/bin/sh
#

set -e
tmpFile=$(mktemp)
gcc main.c -o $tmpFile -lcurl -lcrypto 
exec "$tmpFile" "$@"
