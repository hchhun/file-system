all:
	gcc -DDISKINFO -o diskinfo parts.c 
	gcc -DDISKLIST -o disklist parts.c 
	gcc -DDISKGET -o diskget parts.c
	gcc -DDISKPUT -o diskput parts.c

diskinfo: parts.c
	gcc -DDISKINFO diskinfo.c -o diskinfo

disklist: parts.c
	gcc -DDISKLIST disklist.c -o disklist

diskget: parts.c
	gcc -DDISKGET diskget.c -o diskget

diskput: parts.c 
	gcc -DDISKPUT diskput.c -o diskput

clean:
	rm -f *.o
	rm -f diskinfo
	rm -f disklist
	rm -f diskget
	rm -f diskput
