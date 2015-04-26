BEGIN { 
	FS=" "
	cur = mx = 0 
}

$1=="malloc" { 
	cur += $2
       	if (cur > mx) 
		mx = cur
       	m[$3] = $2
}

$1=="free" { 
	if (m[$2] == "")
		print $0": pointer is not allocated"
       	cur -= m[$2] 
	delete m[$2] 
}

END { 
	print "peak memory: "mx
	for (p in m)
		print "leak: "p"="m[p]
} 
