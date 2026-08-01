/*
 * Camada 1: servidor TCP bloqueante.
 *
 * Este programa NAO interpreta o protocolo HTTP de verdade: ele apenas
 * aceita uma conexao TCP por vez, le o que o cliente mandar, ignora o
 * conteudo e devolve sempre a mesma resposta HTTP 200 com o header
 * "Connection: close". A ideia e servir de base para camadas futuras
 * (parsing de requisicao, roteamento, etc.).
 */

#include <stdio.h>      /* perror, snprintf */
#include <stdlib.h>     /* exit, EXIT_FAILURE */
#include <string.h>     /* memset */
#include <unistd.h>     /* close, read, write */
#include <arpa/inet.h>  /* htons, INADDR_ANY, struct sockaddr_in */
#include <sys/socket.h> /* socket, bind, listen, accept, setsockopt */

#define PORT 8080
#define BACKLOG 128
#define REQUEST_BUFFER_SIZE 4096

/* Corpo fixo devolvido para qualquer requisicao recebida. */
static const char *BODY = "Hello from my C server!";

/*
 * Monta a resposta HTTP completa (status line + headers + corpo) em
 * 'response', usando snprintf para nao estourar o buffer.
 * Retorna o numero de bytes escritos (sem contar o terminador nulo).
 */
static int build_response(char *response, size_t response_size) {
    return snprintf(
        response,
        response_size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        strlen(BODY),
        BODY
    );
}

int main(void) {
    /*
     * socket(): cria um endpoint de comunicacao e devolve um file
     * descriptor para ele.
     *   AF_INET     -> familia de enderecos IPv4.
     *   SOCK_STREAM -> socket orientado a conexao (TCP).
     *   0           -> protocolo padrao para essa combinacao (TCP).
     */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /*
     * setsockopt(SO_REUSEADDR): permite fazer bind na porta mesmo que
     * ela esteja em estado TIME_WAIT por causa de uma execucao anterior
     * do servidor. Sem isso, reiniciar o servidor rapidamente costuma
     * falhar com "Address already in use".
     */
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * Estrutura que descreve o endereco/porta em que o socket vai
     * escutar. Precisa ser zerada antes de preencher para nao deixar
     * lixo de memoria nos campos nao usados (ex.: sin_zero).
     */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;         /* IPv4 */
    server_addr.sin_addr.s_addr = INADDR_ANY; /* escuta em todas as interfaces locais */
    server_addr.sin_port = htons(PORT);       /* htons: converte porta de host order para network order (big-endian) */

    /*
     * bind(): associa o socket ao endereco/porta definidos acima.
     * E o que reserva a porta 8080 para este processo.
     */
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * listen(): transforma o socket em um socket passivo, pronto para
     * aceitar conexoes de entrada. O segundo argumento (backlog) e o
     * tamanho maximo da fila de conexoes que ja completaram o
     * three-way handshake mas ainda nao foram aceitas via accept().
     */
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    printf("Servidor escutando na porta %d...\n", PORT);

    char request_buffer[REQUEST_BUFFER_SIZE];
    char response_buffer[REQUEST_BUFFER_SIZE];

    /* Loop infinito: o servidor atende uma conexao por vez, de forma bloqueante. */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        /*
         * accept(): bloqueia ate que um cliente complete uma conexao
         * na fila de pendentes criada pelo listen(). Devolve um NOVO
         * file descriptor, exclusivo para essa conexao; o listen_fd
         * continua livre para aceitar as proximas conexoes.
         */
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            perror("accept");
            continue; /* nao mata o servidor por causa de uma conexao que falhou */
        }

        /*
         * read(): bloqueia ate o cliente enviar dados (ou fechar a
         * conexao). Aqui so precisamos consumir a requisicao da rede;
         * nao fazemos parsing de metodo/path/headers nesta camada.
         */
        ssize_t bytes_read = read(client_fd, request_buffer, sizeof(request_buffer) - 1);
        if (bytes_read < 0) {
            perror("read");
            close(client_fd);
            continue;
        }
        request_buffer[bytes_read] = '\0';

        /* Monta a resposta HTTP 200 fixa no buffer de saida. */
        int response_len = build_response(response_buffer, sizeof(response_buffer));

        /*
         * write(): envia a resposta para o cliente atraves do socket
         * da conexao. Em um servidor robusto seria preciso checar se
         * write() escreveu menos bytes do que o pedido e reenviar o
         * restante em loop; aqui, para uma resposta pequena, uma
         * unica chamada e suficiente na pratica.
         */
        if (write(client_fd, response_buffer, (size_t)response_len) < 0) {
            perror("write");
        }

        /*
         * close(): encerra a conexao TCP com o cliente (envia FIN).
         * Como respondemos com "Connection: close", fechar aqui e
         * coerente com o header que prometemos ao cliente.
         */
        close(client_fd);
    }

    /* Inalcancavel: o loop acima so termina por sinal/kill do processo. */
    close(listen_fd);
    return 0;
}
