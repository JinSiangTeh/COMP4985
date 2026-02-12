
How to compile:
Server: gcc -o server main.c -lpthread -lncurses
Client: gcc -o client client.c -lpthread -lncurses

Server: ./server myport managerIP managerPort
Client: ./client serverManagerIP serverManagerPort


