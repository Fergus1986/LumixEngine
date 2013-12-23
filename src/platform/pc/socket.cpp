#include "platform/socket.h"
#include <Windows.h>

#ifndef DISABLE_NETWORK

#pragma comment(lib,"Ws2_32.lib")

namespace Lux
{
	namespace Net
	{
		struct SocketImpl
		{
			SOCKET m_socket;
		};

		Socket::Socket()
		{
			m_implmentation = new SocketImpl;
		}

		Socket::~Socket()
		{
			delete m_implmentation;
		}

		bool Socket::init()
		{
			WORD sockVer;
			WSADATA wsaData;
			sockVer = MAKEWORD(2,2);
			return WSAStartup(sockVer, &wsaData) == 0;
		}

		bool Socket::create(const char* ip, uint16_t port)
		{
			m_implmentation->m_socket = ::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
			if(m_implmentation->m_socket == INVALID_SOCKET)
			{
				return false;
			}

			SOCKADDR_IN sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_port = htons(port);
			sin.sin_addr.s_addr = ip ? ::inet_addr(ip) : INADDR_ANY;

			int retVal = ::bind(m_implmentation->m_socket, (LPSOCKADDR)&sin, sizeof(sin));
			int err = ::WSAGetLastError();
			if(retVal == SOCKET_ERROR)
			{
				ASSERT(false);
				return false;
			}

			return ::listen(m_implmentation->m_socket, 10) == 0;		
		}

		Socket* Socket::accept()
		{
			Socket* s = new Socket;
			s->m_implmentation->m_socket = ::accept(m_implmentation->m_socket, NULL, NULL);

			return s;
		}

		Socket* Socket::connect(const char* ip, uint16_t port)
		{
			m_implmentation->m_socket = ::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
			if(m_implmentation->m_socket == INVALID_SOCKET)
			{
				return NULL;
			}

			SOCKADDR_IN sin;

			memset (&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_port = htons(port);
			sin.sin_addr.s_addr = ip ? ::inet_addr(ip) : INADDR_ANY; 
			
			if (::connect(m_implmentation->m_socket, (LPSOCKADDR)&sin, sizeof(sin)) != 0) 
			{
				ASSERT(false);
				return NULL;
			}
			
			Socket* s = new Socket;
			s->m_implmentation->m_socket = m_implmentation->m_socket;

			return s;
		}

		bool Socket::send(const void* data, int size)
		{
			return ::send(m_implmentation->m_socket, static_cast<const char*>(data), size, 0) == size;
		}

		int Socket::receive(void* data, int size)
		{
			return ::recv(m_implmentation->m_socket, static_cast<char*>(data), size, 0);
		}

		bool Socket::canReceive()
		{
			u_long x;
			ioctlsocket(m_implmentation->m_socket, FIONREAD, &x);
			return x > 0;
		}

		bool Socket::receiveAllBytes(void* data, int size)
		{
			int to_receive = size;
			char* ptr = static_cast<char*>(data);

			do
			{
				int received = receive(ptr, to_receive);
				ptr += received;
				to_receive -= received;
				if(received <= 0)
				{
					if(WSAGetLastError() == WSAEWOULDBLOCK)
					{
						ptr -= received;
						to_receive += received;
					}
					else
					{
						return false;
					}
				}
			} while(to_receive > 0);
			return true;
		}
	} // ~namespace Net
} // ~namespace Lux


#endif // DISABLE_NETWORK