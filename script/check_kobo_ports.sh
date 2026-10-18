# fails when host paths leak into the ports' .pc and .la files
# usage: bash check_kobo_ports.sh <PORTBUILD_PATH>
set -e
P=$1
bad=0
for f in $(find "$P" \( -name '*.pc' -o -name '*.la' \)); do
    # the ports' own prefix is legitimate (it sits under the runner's home)
    if sed "s#$P##g" "$f" | grep -nE "/usr/(include|lib)|$HOME"; then
        echo "## host path leaked into $f"; bad=1
    fi
done
exit $bad
