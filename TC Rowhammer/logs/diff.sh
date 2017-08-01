triplets=$(grep tfl "$1" | tr -d '()[]' | tr , ' ' | cut -d' ' -f1,2,4 | sort | uniq)
while read -r triplet; do
	read -r t1 t2 v <<< "$triplet"
	read -r -d '' cmd <<-EOCMD
		print(hex(abs($t2-$t1))+" "+hex(abs($v-$t2))+" "+hex(abs($v-$t1)))
	EOCMD
	tdiff=$(python -c "$cmd")
	echo "$tdiff" | sort -k1
done <<< "$triplets"
