#include "api.h"

struct Session { 
  int id; 
  int req_pipe; 
  int notif_pipe; 
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];        
};
        
static struct Session session = {.id = -1};
    
int pacman_connect(const char *req_pipe_path, const char *notif_pipe_path, const char *server_pipe_path) { 
        
    // limpar pipes antigos (caso existam)
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    
    // criar os FIFOs do cliente
    mkfifo(req_pipe_path, 0666); mkfifo(notif_pipe_path, 0666);
    
    // abrir FIFO de registo do servidor 
    int fd_server = open(server_pipe_path, O_WRONLY); 
    
    if (fd_server < 0) return 1; // construir mensagem de pedido segundo o protocolo 
    
    char msg[81];
    
    memset(msg, 0, sizeof(msg)); 
    
    msg[0] = 1; // OP_CONNECT 
    
    strncpy(msg + 1, req_pipe_path, 40); 
    strncpy(msg + 41, notif_pipe_path, 40);  
    
    //enviar pedido
    
    if (write(fd_server, msg, sizeof(msg)) != sizeof(msg)) {close(fd_server); return 1; } 
    
    close(fd_server);
    
    // abrir FIFOs da sessão 
    session.req_pipe = open(req_pipe_path, O_WRONLY); 
    
    if (session.req_pipe < 0) return 1; 
    
   int fd_notif = open(notif_pipe_path, O_RDONLY); 

   session.notif_pipe = fd_notif;  

    if (fd_notif < 0) return 1; 
    
    // esperar confirmação do servidor 
    char reply[2]; 
    
    if (read(fd_notif, reply, 2) != 2) return 1;
    
    if (reply[0] != 1 || reply[1] != 0) return 1; 
    
    // guardar paths na sessão
    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH); 
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    return 0; 
} 


void pacman_play(char command) { 
    
    if (session.req_pipe < 0) return; // sessão não ativa 
    
    char msg[2]; 
    
    msg[0] = 3; // OP_CODE

    msg[1] = command; // comando do Pacman 
    
    if (write(session.req_pipe, msg, sizeof(msg)) != sizeof(msg)) return; 
    
    return; 
 } 


int pacman_disconnect() { 
    
    if (session.req_pipe < 0 && session.notif_pipe < 0) return 1; // nenhuma sessão ativa
    
    // enviar pedido de desconexão
    char op = OP_CODE_DISCONNECT;
    
    if (session.req_pipe >= 0) {
        signal(SIGPIPE, SIG_IGN);
        write(session.req_pipe, &op, 1);
    } 
    
    // fechar FIFOs 
    if (session.req_pipe >= 0) close(session.req_pipe); 
    
    if (session.notif_pipe >= 0) close(session.notif_pipe); 
    
    session.req_pipe = -1; session.notif_pipe = -1;
    
    // apagar named pipes 
    if (session.req_pipe_path[0]) unlink(session.req_pipe_path); 
    
    if (session.notif_pipe_path[0]) unlink(session.notif_pipe_path); 
    
    return 0; 
}


Board receive_board_update(void)
{   
    Board board;

    memset(&board, 0, sizeof(Board));

    if (session.notif_pipe < 0) {
        debug("receive: notif_pipe < 0\n"); 
        return board;
    }

    // ler opcode
    char opcode;
    ssize_t lidos = read(session.notif_pipe, &opcode, 1);
    if (lidos <= 0) {
        debug("receive_board: erro ao ler opcode\n");
        return board;
    }

    if ((unsigned char)opcode != OP_CODE_BOARD) {
        debug("receive_board: opcode errado %d\n", (unsigned char)opcode);
        return board;
    }

    // ler os 6 inteiros do header
    int header[6];
    ssize_t bytes_lidos = 0;
    while (bytes_lidos < (ssize_t)sizeof(header)) {
        lidos = read(session.notif_pipe, ((char*)header) + bytes_lidos, sizeof(header) - bytes_lidos);
        if (lidos <= 0) {
            debug("receive_board: erro ao ler header\n");
            return board;
        }
        bytes_lidos += lidos;
    }

    board.width = header[0];
    board.height = header[1];
    board.tempo = header[2];
    board.victory = header[3];
    board.game_over = header[4];
    board.accumulated_points = header[5];

    // alocar memoria para o tabuleiro
    board.data = malloc(board.width * board.height);
    if (!board.data) return board;

    // ler dados do tabuleiro
    ssize_t tamanho = board.width * board.height;
    ssize_t pos = 0;
    while (pos < tamanho) {
        ssize_t r = read(session.notif_pipe, board.data + pos, tamanho - pos);
        if (r <= 0) {
            free(board.data);
            board.data = NULL;
            return board;
        }
        pos += r;
    }

    return board;
}