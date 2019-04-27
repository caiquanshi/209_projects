#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 58966
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr);
void remove_player(struct client **top, int fd);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf);
void announce_turn(struct game_state *game);
void announce_winner(struct game_state *game, struct client *winner);
/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game);


/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}

void broadcast(struct game_state *game, char *outbuf){
    struct client *p;
    for(p = game->head; p != NULL; p = p->next) {
        write(p->fd, outbuf, strlen(outbuf));
    }
}

void announce_turn(struct game_state *game){
    struct client *p;
    for(p = game->head; p != NULL; p = p->next) {
        if (p != game->has_next_turn){
            char word[MAX_BUF];
            sprintf(word, "it's %s's turn\r\n", (game->has_next_turn)->name);
            write(p->fd, word, strlen(word));
        }else{
            char *mess = "your guess\r\n";
            write(p->fd, mess, strlen(mess));
        }
    }
    printf("it's %s's turn\n", (game->has_next_turn)->name);
}

void announce_winner(struct game_state *game, struct client *winner){
    struct client *p;
    for(p = game->head; p != NULL; p = p->next) {
        if (p != winner){
            char word[MAX_BUF];
            sprintf(word, "Game over! %s won!\r\n", winner->name);
            write(p->fd, word, strlen(word));
        }else{
            char *mess = "Game over! You won!\r\n";
            write(p->fd, mess, MAX_BUF);
        }
    }
    printf("Game over! %s won!\n", winner->name);
}

void advance_turn(struct game_state *game){
    game->has_next_turn = game->has_next_turn->next;
    if (game->has_next_turn == NULL){
        game->has_next_turn = game->head;
    }
}

int buf_read(char *word, struct client *p){

    int nbytes = read(p->fd, p->in_ptr, MAX_BUF);
    printf("Read %d bytes\n", nbytes);
    if (nbytes == -1){
        perror("read");
        exit(1);
    }
    (p->in_ptr)[nbytes] = '\0'; 
    if (nbytes == 0){
        // the play leave return -1
        return -1;
    }
    p->in_ptr += nbytes;
    char *ptr = NULL;
    if ((ptr = strstr(p->inbuf, "\r\n")) != NULL){
        
        *(ptr) = '\0';
        printf("find new line %s\n", p->inbuf);
        strcpy(word, p->inbuf);
        memset(p->inbuf, '\0', MAX_BUF);
        p->in_ptr = p->inbuf;
        // new line found, return length of the word
        return strlen(word);
    }
    // -2 represent no net work new line find during this read
    return -2;
}


int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;

    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, clientfd);
            }
        }
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */

        for(int cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                for(p = game.head; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        if (p ==  game.has_next_turn){
                            //when play have turn input value
                            char ca[MAX_BUF];
                            int read = buf_read(ca, p);
                            if (read == -2){
                                // not have finished line just break wait for next turn to read
                                break;
                            }
                            if (read == -1){
                                //player have turn quit
                                advance_turn(&game);
                                char name[MAX_NAME];
                                strcpy(name, p->name);
                                printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                                remove_player(&(game.head), p->fd);
                                char mesg[MAX_NAME + 9];
                                sprintf(mesg, "goodbye %s\r\n", name);
                                broadcast(&game, mesg);
                                announce_turn(&game);
                                break;
                            }else if (read != 1 ||(*ca < 'a' || *ca > 'z')){
                                // what we do when input is nor vailed
                                char *mess = "invailed input\r\nyour guess\r\n";
                                write(p->fd, mess, strlen(mess));
                                break;
                            }else{
                                // when input is vailde
                                int what_happen = guess_done(&game, *ca);
                                if (what_happen == -1){
                                    //when input is already have
                                    char *mess = "guess already have\r\nyour guess?\r\n";
                                    write(p->fd, mess, strlen(mess));
                                }else if(what_happen == 0){
                                    // wrong guss
                                    char messg[MAX_BUF];
                                    printf("%s's guess is %s\n", p->name, ca);
                                    sprintf(messg, "%s's guess is %s\r\n", p->name, ca);
                                    broadcast(&game, messg);
                                    memset(messg, '\0', MAX_BUF);
                                    sprintf(messg, "Letter %s is not in the word\r\n", ca);
                                    printf("Letter %s is not in the word\n", ca);
                                    write(p->fd, messg, strlen(messg));
                                    broadcast(&game, status_message(messg, &game));
                                    advance_turn(&game);
                                    announce_turn(&game);
                                }else if(what_happen == 1){
                                    //this player win
                                    announce_winner(&game, p);
                                    init_game(&game,argv[1]);
                                    advance_turn(&game);
                                    printf("New Game\n");
                                    broadcast(&game, "\r\n\r\n\r\n");
                                    announce_turn(&game);
                                }else if(what_happen == 3) {
                                    // right guess
                                    char messg[MAX_BUF];
                                    sprintf(messg, "%s's guess is %s\r\n", p->name, ca);
                                    broadcast(&game, messg);
                                    broadcast(&game, status_message(messg, &game));
                                    announce_turn(&game);
                                }else{
                                    // no guess left
                                    char mess[MAX_BUF];
                                    broadcast(&game, status_message(mess, &game));
                                    memset(mess, '\0', MAX_BUF);
                                    sprintf(mess, "haha, Game over!\r\nThe word is %s\r\nplay new game\r\n", game.word);
                                    broadcast(&game, mess);
                                    init_game(&game,argv[1]);
                                    advance_turn(&game);
                                    printf("New Game\n");
                                    broadcast(&game, "\r\n\r\n\r\n");
                                    announce_turn(&game);
                                }
                            }
                        }else{
                            //when player don't have turn input value
                            char ca[MAX_BUF];
                            int read = buf_read(ca, p);
                            if (read == -2){
                                //wait for next iteration to read
                                break;
                            }
                            if(read == -1){
                                // player don't have turn quit
                                char name[MAX_NAME];
                                strcpy(name, p->name);
                                printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                                remove_player(&(game.head), p->fd);
                                char mesg[MAX_NAME + 9];
                                sprintf(mesg, "goodbye %s\r\n", name);
                                broadcast(&game, mesg);
                                announce_turn(&game);
                            }else{
                                // player input value 
                                char *mess = "it's not your turn\r\n";
                                write(p->fd, mess, strlen(mess));
                                printf("%s tried to gues out of turn\n", p->name);
                            } 
                        }
                        break;
                    }
                }
        
                // Check if any new players are entering their names
                for(p = new_players; p != NULL; p = p->next) {
                    if(cur_fd == p->fd) {
                        char name[MAX_NAME];
                        int name_len = buf_read(name, p);
                        if (name_len == -1){
                            printf("Disconnect from %s\n", inet_ntoa(q.sin_addr));
                            remove_player(&new_players, p->fd);
                            break;
                        }
                        if (name_len == -2){
                            break;
                        }
                        if (name_len == 0){
                            char *mess = "Name can not be empty\r\n What is your name?\r\n";
                            write(p->fd, mess, strlen(mess));
                            break;
                        }
                        if (name_len >= MAX_NAME){
                            char *mess = "Name too long\r\n What is your name?\r\n";
                            write(p->fd, mess, strlen(mess));
                            break;
                        }
                        int compare;
                        int count = 0;
                        struct client *ap;
                        for(ap = game.head; ap != NULL; ap = ap->next) {
                            compare = strcmp(name, ap->name);
                            if (compare == 0){
                                char *mess = "Name already exsits\r\nWhat is your name?\r\n";
                                write(p->fd, mess, strlen(mess));
                                count = 1;
                                break; 
                            }
                        }
                        if (count == 1){
                            break;
                        }
                        strcpy(p->name, name);
                        if (p == new_players){
                            new_players = p->next;
                        }else{
                            for(ap = game.head; ap != NULL; ap = ap->next) {
                                if (ap->next == p){
                                    ap->next = p->next;
                                    break;
                                }
                            }
                        }
                        if(game.head == NULL){
                            game.has_next_turn = p;
                        }
                        p->next = game.head;
                        game.head = p;
                        char mesg[MAX_BUF];
                        sprintf(mesg,"%s have just joined\r\n", p->name);
                        broadcast(&game, mesg);
                        status_message(mesg, &game);
                        write(p->fd, mesg, strlen(mesg));
                        announce_turn(&game);
                        break;
                    }  
                }
            }
        }
    }
    return 0;
}


