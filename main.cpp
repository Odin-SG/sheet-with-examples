#include <iostream>
#include <set>
#include <string.h>
#include <algorithm>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

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

int main(int argc, char* argv[]){
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
