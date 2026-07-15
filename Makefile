all: mini_web_server request_worker

mini_web_server: obj/config.o obj/http_response.o obj/log.o obj/user_store.o obj/user_index.o obj/main.o obj/request_handler.o obj/process_server.o obj/tcp_server.o obj/tcp_fork_server.o obj/thread_pool.o obj/tcp_pool_server.o
	gcc -g -o mini_web_server obj/config.o obj/http_response.o obj/log.o obj/user_store.o obj/user_index.o obj/main.o obj/request_handler.o obj/process_server.o obj/tcp_server.o obj/tcp_fork_server.o obj/thread_pool.o obj/tcp_pool_server.o -lm -lpthread

request_worker: obj/request_worker.o obj/log.o obj/user_store.o obj/user_index.o obj/request_handler.o obj/http_response.o
	gcc -g -o request_worker obj/request_worker.o obj/log.o obj/user_store.o obj/user_index.o obj/request_handler.o obj/http_response.o -lm -lpthread

obj/config.o: src/config.c include/config.h
	gcc -g -I./include -c src/config.c -o obj/config.o

obj/http_response.o: src/http_response.c include/http_response.h
	gcc -g -I./include -c src/http_response.c -o obj/http_response.o

obj/log.o: src/log.c include/log.h
	gcc -g -I./include -c src/log.c -o obj/log.o

obj/user_store.o: src/user_store.c include/user_store.h include/user_index.h
	gcc -g -I./include -c src/user_store.c -o obj/user_store.o

obj/user_index.o: src/user_index.c include/user_index.h include/user_store.h
	gcc -g -I./include -c src/user_index.c -o obj/user_index.o

obj/main.o: src/main.c include/config.h include/http_response.h include/log.h include/process_server.h include/tcp_fork_server.h include/tcp_pool_server.h include/user_store.h
	gcc -g -I./include -c src/main.c -o obj/main.o

obj/request_handler.o: src/request_handler.c include/request_handler.h include/http_response.h include/log.h include/user_store.h
	gcc -g -I./include -c src/request_handler.c -o obj/request_handler.o

obj/process_server.o: src/process_server.c include/process_server.h include/log.h include/ipc_utils.h
	gcc -g -I./include -c src/process_server.c -o obj/process_server.o

obj/tcp_server.o: src/tcp_server.c include/tcp_server.h include/config.h include/log.h include/request_handler.h
	gcc -g -I./include -c src/tcp_server.c -o obj/tcp_server.o

obj/tcp_fork_server.o: src/tcp_fork_server.c include/tcp_fork_server.h include/log.h include/request_handler.h
	gcc -g -I./include -c src/tcp_fork_server.c -o obj/tcp_fork_server.o

obj/thread_pool.o: src/thread_pool.c include/thread_pool.h include/log.h include/request_handler.h
	gcc -g -I./include -c src/thread_pool.c -o obj/thread_pool.o

obj/tcp_pool_server.o: src/tcp_pool_server.c include/tcp_pool_server.h include/thread_pool.h include/log.h include/request_handler.h
	gcc -g -I./include -c src/tcp_pool_server.c -o obj/tcp_pool_server.o

obj/request_worker.o: src/request_worker.c include/request_handler.h include/log.h include/user_store.h include/ipc_utils.h
	gcc -g -I./include -c src/request_worker.c -o obj/request_worker.o

clean:
	rm -f obj/*.o mini_web_server request_worker
