#! /bin/bash

set -e

while getopts ih:nvp:dc:t:s:u:P:CNSlTUIf: opt; do
    case $opt in
    f)
	filename=$OPTARG
	;;
    *)
	opts=(${opts[@]} -$opt $OPTARG)
	;;
    esac
done
shift $(($OPTIND - 1))
dbname=$1

tps=$(psql -At -c "SELECT count(*) FROM branches" $dbname)

vacuumdb -t branches $dbname
vacuumdb -t tellers $dbname
psql -c "DELETE FROM history" $dbname
vacuumdb -t history $dbname

if [ -z $filename ]; then
    pgcbench ${opts[@]} $@
else
    perl -pe "BEGIN { \$tps = $tps } s/\`([^\`]+)\`/eval \$1/eg" $filename \
	| pgcbench ${opts[@]} -f - $@
fi
