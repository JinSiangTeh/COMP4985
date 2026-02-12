IP: 192.168.0.87
Port: 9000 /8080 Not sure just try both


How to compile:
Server: gcc -o server main.c -lpthread -lncurses
Client: gcc -o client client.c -lpthread -lncurses

Server: ./server myport managerIP managerPort
Client: ./client serverIP serverPort


