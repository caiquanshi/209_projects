FLAGS = -Wall -std=gnu99 -g

all : psort

testp: testp.o helper.o
	gcc ${FLAGS} -o $@ $^

%.o: %.c 
	gcc ${FLAGS} -c $<

psort: psort.o helper.o
	gcc ${FLAGS} -o $@ $^

clean :
	rm *.o psort mkwords testp
