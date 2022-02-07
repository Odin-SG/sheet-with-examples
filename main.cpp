#include <iostream>
#include <set>
#include <string.h>
#include <algorithm>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>


#define POLL_SIZE 2048

using namespace std;

// Аллокатор
class SmallAllocator {
private:
    char Memory[1048576]; // Внетренний буфер в 1 мб
    int offset; // Смещение относительно верхушки. Указатель на вершину памяти
public:
    SmallAllocator():offset(0){} // Выставляем в 0
    void *Alloc(unsigned int Size) { // Выделяем память
        offset += Size; // Резервируем SIze байт для данных
        if (offset < 1048576) // Если не вышли за пределы
            return (void*) ((char *) &Memory[offset]- Size); // Возвращаем воид указатель
            // Из мемори с нашим смещением (верхушкой данных) вычитаем тот самый Size, который мы заранее добавили
        else{ // Есди вышли за пределы
            offset -= Size; // Просто вычитаем Size и возвращаем нуль
            return nullptr;
        }
    };
    void *ReAlloc(void *Pointer, unsigned int Size) { // Выделение памяти бОльшего объема
        if (offset + Size >= 1048576) // Если привысили
            return nullptr; // возвращаем нуль
        for (int counter = 0; counter < Size; counter++) // Ходим по памяти от 0 до Size
            *(&Memory[offset] + counter) = *((char *)Pointer + counter); // Копируем каждый байт из старого указателя в новую область памяти
        offset += Size; // Указатель на вершину памяти теперь + Size
        return (void*) ((char *) &Memory[offset] - Size); // Возвращяем указатель на начало выделенной памяти
    };
    void Free(void *Pointer) {offset = 0;} // Чистим всёёёё
};

int set_nonblock(int fd) // взято из тырнета
{
    int flags;
#if defined(O_NONBLOCK)
    if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
    flags = 1;
    return ioctl(fd, FIOBIO, &flags);
#endif
}

int selectSocket(){
	int masterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	std::set<int> slaveSockets;
	char buffer[2048]; // Создаём буфер на 2кб для чтения из сокета

	if(masterSocket == -1)
		perror("Socket not allowed\n");

	int flagReuseaddr = 1;
	if (setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, &flagReuseaddr, sizeof(flagReuseaddr)) == -1) // Устанавливаем мастер сокет в режим переиспользования адреса
		perror("Not allowed reuse addr\n");

	struct sockaddr_in serverAddr; // Выделяем структуру sockaddr для INET
	serverAddr.sin_family = AF_INET; // Протокол IP
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Биндимся ко всем доступным интерфейсам
	serverAddr.sin_port = htons(55001); // К этому порту
	if(bind(masterSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1){
		perror("Not allowed bind to addr\n");
	}

	set_nonblock(masterSocket); // Ставим в неблокирующий режим, чтоб мастер сокет мог принимать не завистая на accept
	if(listen(masterSocket, SOMAXCONN) == -1){
		perror("Not allowed listen socket\n");
	}

	while(true){
		fd_set sets; // Создаём структуру fd_set. Там хранятся биты установленные в 1, порядковый номер бита соответсвет величине дескриптора, которыц мы туда засунем.
		//Остальные установлены в 0. (Дескриптор численно равен 4, значит 4-ый бит равен 1. И так со всеми. Максимум 1024)
		FD_ZERO(&sets); // Каждый раз обнуляем sets, так как select возвращает туда биты с дескрипторами из которых можно читать
		FD_SET(masterSocket, &sets); // Запихиваем мастера в sets, чтоб можно было отслеживать запросы на подключение
		for(auto iter: slaveSockets){
			FD_SET(iter, &sets); // В цикле дописываем туда дескрипторы слэйв сокетов
		}
		int max = std::max(masterSocket, *std::max_element(slaveSockets.begin(), slaveSockets.end())); // Ищем численный максимум от мастера до последнего слэйв сокета
		select(max+1, &sets, NULL, NULL, NULL); // Спим, пока из ядра не придёт событие чтение из сокета.
		//В sets установленные в 1 биты означают теперь те сокеты, из которых можно читать

		for(auto readSock: slaveSockets){ // Бежим по слейв сокетам через итераторы
			if(FD_ISSET(readSock, &sets)){ // Если бит данного раба установлен в сетс в 1, значит оттуда можно читать
				memset(buffer, 0, sizeof(buffer)); // Обнуляем буффер. Либо его можно сделать ststic, тогда обнулять не нужно

				int recvSize;
				if((recvSize = recv(readSock, buffer, 1024, MSG_NOSIGNAL)) == 0 && errno != EAGAIN){ //Если в буфер пришло 0 байт и ошибка не равна повторной попытке чтения - убиваем соединение
					shutdown(readSock, SHUT_RDWR); // Отправляем FIN
					close(readSock); // Закрываем дескриптор
					slaveSockets.erase(readSock); // Выпиливаем дескриптор сокета из нашего дерева
				} else if(recvSize != 0){ // Если объем данных не нулевой
					cout << buffer; // Пишем в stdout пришедшее сообщение
					for(auto otherSocks: slaveSockets){ // Бегаем итератором по дереву с дескриптораи клиентских сокетов
						if(readSock != otherSocks)  // Если сокет с которого пришли данные не равен остальны клиентски сокета
							send(otherSocks, buffer, recvSize, MSG_NOSIGNAL); // Отправляем остальным клиентам
					}
				}
			}
		}
		if(FD_ISSET(masterSocket, &sets)){ // Если бит мастер сокета установлен в 1, значит пришел новый запрос на подключение (мастер только принимает соединения)
			int slaveSocket = accept(masterSocket, 0, 0); // Принимаем дескриптор соединения
			set_nonblock(slaveSocket); // Делаем его неблокирующим
			slaveSockets.insert(slaveSocket); // Пихаем нового клиента в дерево
		}
	}
	return 0;
}

int pollSocket () {
	int masterSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); // Создаём мастер сокет
	std::set<int> slaveSockets; // Создаём сет для воркеров
	char buffer[2048]; // Создаём буффер для сообщений

	if(masterSocket == -1) // Проверяем на ошибки
		perror("Error where open socket\n");

	int flagReuseaddr = 1; // Устанавливаем сокет в reuseaddr. Чтоб можно было переисользовать порт сразу, а не ждать пока он закроется
	if(setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, &flagReuseaddr, sizeof(flagReuseaddr)) == -1){
		perror("Error where set reuseaddr\n");
	}
	set_nonblock(masterSocket); // Ставим в неблокирующий режим, чтоб мастер сокет мог принимать не завистая на accept

	struct sockaddr_in serverAddr; // Создаём структуру с адресом нашего сервера
	serverAddr.sin_family = AF_INET; // Указываем протокол IP
	inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr); // Указываем адрес, который будем слушать
	serverAddr.sin_port = htons(55002); // Указываем порт, на котором будем висеть

	if(bind(masterSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) == -1) // Биндим мастер сокет на наш адрес
		perror("Error where bind to addr\n");

	if(listen(masterSocket, SOMAXCONN) == -1) // Слушаем адрес
		perror("Error where listen socket\n");

	pollfd pollArray[POLL_SIZE]; // Создаём массив структур pollfd. В ней хранятся дескрипторы, события и возвращённые события

	pollArray[0].fd = masterSocket; // Нулевой элемент - это мастер сокет
	pollArray[0].events = POLLIN; // Он обрабатывает только POLLIN событие

	while(true){
		int indexSlavePollArray = 1; // Индекс внутри нашего массива pollArray
		for(auto slaveSocket: slaveSockets){ // Итерируемся по дереву и пихаем в pollArray всех клиентов (втч новых, если кто-то подключился за предыдущий цикл)
			pollArray[indexSlavePollArray].fd = slaveSocket; // Дескриптор
			pollArray[indexSlavePollArray].events = POLLIN; // Событие
			indexSlavePollArray++;
		}
		uint32_t sizePollArray = slaveSockets.size() + 1; // Чтобы не бегать по всем 2048 элементам, вычисляем размер (это необходимо для poll)

		poll(pollArray, sizePollArray, -1); // Ждём пока полл вернёт событие в массив pollArray, размера sizePollArray. Ждать будем бесконечно

		for(int concreteDescriptor = 0; concreteDescriptor < sizePollArray; concreteDescriptor++){ // Ходим по всем элементам, от 0 до sizePollArray
			if(pollArray[concreteDescriptor].revents & POLLIN) { // Если в конкретном обработчике pollArray[concreteDescriptor] вернулось событие POLLIN, то обрабатываем его
				if(concreteDescriptor) { // Если его индекс не равен 0 (0 это мастер, он принимает новые соединения)
					memset(buffer, 0, sizeof(buffer)); // Обнуляем память
					int sizeRecv = recv(pollArray[concreteDescriptor].fd, buffer, sizeof(buffer), MSG_NOSIGNAL); // Читаем из данного дескриптора
					if(sizeRecv == 0 && errno != EAGAIN){ // Если данных нет, и ошибка не равна повторной попытке чтения из дескриптора, то убиваем сединение
						shutdown(pollArray[concreteDescriptor].fd, SHUT_RDWR); // Кидаем FIN
						slaveSockets.erase(pollArray[concreteDescriptor].fd); // Убираем из дерева
						close(pollArray[concreteDescriptor].fd); // Полностью освобождаем десприктор
					} else if (sizeRecv > 0){ // Если что-то пришло
						for(auto otherSocket: slaveSockets) { // Итерируемся по всем клиентам
							if(otherSocket != pollArray[concreteDescriptor].fd) // Только если это не отправитель
								send(otherSocket, buffer, sizeof(buffer), MSG_NOSIGNAL); // То отправляем сообщение каждому соединенному клиенту
						}
					}
				} else { // Если это нулево индекс, значит пришёл запрос на соединение
					int slaveSocket = accept(masterSocket, 0, 0); // Отправляем ACK, выполняем рукопожатие ну и короче принимаем соединение
					if(slaveSocket == -1)
						perror("Error where accept\n");
					set_nonblock(slaveSocket); // Устанавливаем в неблокирующий режим, чтобы не висеть на recv
					slaveSockets.insert(slaveSocket); // Пихаем дескриптор нового клиента в дерево
				}
			}
		}
	}

	return 0;
}

int main (int argc, char *argv[]) {
	pollSocket();
	return 0;
}




