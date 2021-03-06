#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <gtk/gtk.h>

#include "crypto.h"
#include "utils.h"
#include "client.h"

#define MAX_LINE 16384

/**
 * Process any events in the event queue
*/
gboolean client_event_loop(Client* this)
{
    if(this != NULL && this->base != NULL)
    {
        event_base_loop(this->base, EVLOOP_NONBLOCK);
        return TRUE;
    }
    return FALSE;
}

void set_tcp_no_delay(evutil_socket_t fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               &one, sizeof one);
    evutil_make_socket_nonblocking(fd);
}

/**
 * Callback for when there is data in the buffer
 * Calls different functions based on the authentication status
*/
void client_readcb(struct bufferevent *bev, void *ctx)
{
    Client *this = ctx;

    switch(this->authState)
    {
        case AUTH_STATE_AUTHENTICATED:
            clientReadStateAuthenticated(this);
            break;
        default:
            clientReadStateNoAuthentication(this);
            break;
    }
}

// The server is authenticated now write out the messages as they come in
void clientReadStateAuthenticated(Client *this)
{
    struct evbuffer *input;
    char *line;
    size_t n;
    input = bufferevent_get_input(this->bev);
    while ((line = evbuffer_readln(input, &n, EVBUFFER_EOL_CRLF_STRICT))) {
        char decryptedMessage[1024] = {};
        char outputBuf[1024] = {};
        decrypt_with_key(line, decryptedMessage, this->sessionKey);
        writeHex(this->cipherTextLog, "Server: ", line, strlen(line));
        sprintf(outputBuf, "Server: %s", decryptedMessage);
        writeLine(this->plainTextLog, outputBuf);
        free(line);
    }
}

// The server should be sending us their public key
// and E("Bob", Ra, g^b mod p, KAB)
void clientReadStateNoAuthentication(Client *this)
{
    struct evbuffer *input;
    char *line;
    size_t len;
    char buf1[1024] = {};
    char buf2[1024] = {};
    input = bufferevent_get_input(this->bev);

    // Rb
    char *serverNonce = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);

    getHex(serverNonce, buf1, NONCE_SIZE);
    sprintf(buf2, "Server: My nonce is %s", buf1);
    writeLine(this->authenticationTextLog, buf2);

    // Encrypted message
    line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF_STRICT);

    getHex(line, buf1, len);
    sprintf(buf2, "Server: My encrypted message is %s", buf1);
    writeLine(this->authenticationTextLog, buf2);

    char decryptedMessage[30] = {};
    decrypt_with_key(line, decryptedMessage,this->sharedPrivateKey);
    free(line);

    char *sender = strtok(decryptedMessage, "\r\n");
    char *returnedNonce = strtok(NULL, "\r\n");
    char *serverDiffieHellmanValue = strtok(NULL, "\r\n");

    sprintf(buf1, "Server: I am %s.\nServer: Diffie-Hellman Value is %s.", sender, serverDiffieHellmanValue);
    writeLine(this->authenticationTextLog, buf1);
    writeHex(this->authenticationTextLog, "Server: Your nonce was ", returnedNonce, NONCE_SIZE);

    if(strcmp(sender, "Server") == 0)
    {
        if(are_nonce_bytes_equal(this->nonce->bytes, returnedNonce))
        {
            this->secretA = get_random_int(DIFFIE_HELLMAN_EXP_RANGE);
            int clientDiffieHellmanVal = (int) pow(DIFFIE_HELLMAN_G, this->secretA) % DIFFIE_HELLMAN_P;
            sprintf(buf1, "Client: g^a mod p: %d with a: %d", clientDiffieHellmanVal, this->secretA);
            writeLine(this->authenticationTextLog, buf1);

            char messageToEncrypt[1024] = {};

            sprintf(messageToEncrypt, "Client\r\n%s\r\n%d", serverNonce, clientDiffieHellmanVal);

            writeHex(this->authenticationTextLog, "Client: ", messageToEncrypt, strlen(messageToEncrypt));

            char encryptedMessage[1024] = {};
            encrypt_with_key(messageToEncrypt, encryptedMessage, this->sharedPrivateKey);

            writeHex(this->authenticationTextLog, "Client: ", encryptedMessage, strlen(encryptedMessage));

            client_send(this, encryptedMessage);

            int dhVal = atoi(serverDiffieHellmanValue);

            // This will be the key used for communication in the future
            int sessionKeyInt = (int) pow(dhVal, this->secretA) % DIFFIE_HELLMAN_P;
            char sessionKeyString[1024] = {};
            sprintf(sessionKeyString, "%d", sessionKeyInt);

            this->sessionKey = key_init_new();
            this->sessionKey->data = get_md5_hash(sessionKeyString, strlen(sessionKeyString));
            this->sessionKey->length = strlen(this->sessionKey->data);

            writeHex(this->authenticationTextLog, "Client: Calculated session key is ", this->sessionKey->data, this->sessionKey->length);

            // The server successfully proved that it is actually the server
            // We can now trust it
            this->authState = AUTH_STATE_AUTHENTICATED;
            this->secretA = 0;
        }
    }
}

/**
 * Called when the initial connection to the server is made
 * When we connect, we start the authentication process by sending
 * the client's nonce
*/
void client_eventcb(struct bufferevent *bev, short events, void *ptr)
{
    Client *this = ptr;
    if (events & BEV_EVENT_CONNECTED)
    {
        gtk_button_set_label(GTK_BUTTON(this->statusButton), "Connected");
        evutil_socket_t fd = bufferevent_getfd(bev);
        set_tcp_no_delay(fd);

        this->nonce = get_nonce();
        char output[1024];
        sprintf(output, "Client: My nonce is %s", this->nonce->hex);
        writeLine(this->authenticationTextLog, output);
        client_send_data(this, this->nonce, NONCE_SIZE);
    }
    else if (events & BEV_EVENT_ERROR)
    {
        gtk_button_set_label(GTK_BUTTON(this->statusButton), "Connect!");
        g_idle_remove_by_data(this);
    }
}

/**
 * Helper function to send a message to the Server
 */ 
void client_send(Client* this, const char *msg)
{
    struct evbuffer *output = bufferevent_get_output(this->bev);
    if(this->sessionKey != NULL)
    {
        char encryptedMessage[1024] = {};
        encrypt_with_key((char *)msg, encryptedMessage, this->sessionKey);
        char buf[1024] = {};
        sprintf(buf, "Client: %s", msg);
        writeLine(this->plainTextLog, buf);
        writeHex(this->cipherTextLog, "Client: ", encryptedMessage, strlen(encryptedMessage));
        evbuffer_add_printf(output, "%s\r\n", encryptedMessage);
    }
    else
    {
        evbuffer_add_printf(output, "%s\r\n", msg);
    }
}

/**
 * Helper function to send data (this may not be valid ASCII) to the Server
 */ 
void client_send_data(Client *this, const void *data, size_t size)
{
    if(this != NULL && this->bev != NULL)
    {
        struct evbuffer *output = bufferevent_get_output(this->bev);
        if(output != NULL)
        {
            evbuffer_add(output, data, size);
        }
    }
}

/**
 * Initializes the client
 * connects to the server's TCP socket
 * Sets up events to work asynchronously
 */
Client* client_init_new(
    GtkWidget *statusButton,
    GtkWidget *plainTextLog,
    GtkWidget *cipherTextLog,
    GtkWidget *portNumber,
    GtkWidget *serverName,
    GtkWidget *sharedKey,
    GtkWidget *authenticationTextLog
)
{
    Client *this = malloc(sizeof(Client));

    this->authenticationTextLog = authenticationTextLog;
    this->plainTextLog = plainTextLog;
    this->cipherTextLog = cipherTextLog;
    this->statusButton = statusButton;
    this->sharedKey = sharedKey;

    this->sessionKey = NULL;

    this->sharedPrivateKey = key_init_new();
    const char *keyText = gtk_entry_get_text(GTK_ENTRY(this->sharedKey));
    this->sharedPrivateKey->length = strlen(keyText);
    this->sharedPrivateKey->data = malloc(this->sharedPrivateKey->length);
    strcpy(this->sharedPrivateKey->data, keyText);

    const char *portNumberString = gtk_entry_get_text(GTK_ENTRY(portNumber));
    int port = atoi(portNumberString);

    const char *serverNameString = gtk_entry_get_text(GTK_ENTRY(serverName));

    this->base = event_base_new();
    if (!this->base)
    {
        printf("Failed to create base");
        client_free(this);
        return NULL;
    }

    memset(&this->sin, 0, sizeof(this->sin));
    this->sin.sin_family = AF_INET;
    inet_aton(serverNameString, &(this->sin.sin_addr));
    this->sin.sin_port = htons(port);

    this->bev = bufferevent_socket_new(this->base, -1, BEV_OPT_CLOSE_ON_FREE);
    if(this->bev == NULL)
    {
        printf("Failed to create bev");
        client_free(this);
        return NULL;
    }

    bufferevent_setcb(this->bev, client_readcb, NULL, client_eventcb, this);
    bufferevent_enable(this->bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect(this->bev,
        (struct sockaddr *)&this->sin, sizeof(this->sin)) < 0)
    {
        printf("Failed to connect");
        client_free(this);
        return NULL;
    }

    return this;
}

/**
 * Free up data allocated in client
 */
void client_free(Client *this)
{
    gtk_button_set_label(GTK_BUTTON(this->statusButton), "Connect!");
    if(this == NULL)
    {
        return;
    }

    g_idle_remove_by_data(this);
    if(this->bev != NULL)
    {
        bufferevent_free(this->bev);
    }
    event_base_free(this->base);
    free(this);
}