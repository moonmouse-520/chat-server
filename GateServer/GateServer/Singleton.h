#pragma once
#include <memory>
#include <iostream>
#include <mutex>

template<typename T>
class Singleton
{
protected:
	Singleton() = default;
	Singleton(const Singleton&) = delete;
	Singleton& operator=(const Singleton&) = delete;
	virtual ~Singleton() {
		std::cout << "this is singleton destruct" << std::endl;
	}

public:
	static std::shared_ptr<T> GetInstance() {
		static std::once_flag s_flag;
		std::call_once(s_flag, [&]() {
			_instance = std::shared_ptr<T>(new T);
			});
		return _instance;
	}

	void PrintAddress() {
		std::cout << _instance.get() << std::endl;
	}
private:
	static std::shared_ptr<T> _instance;
};

template<typename T>
std::shared_ptr<T> Singleton<T>::_instance = nullptr;