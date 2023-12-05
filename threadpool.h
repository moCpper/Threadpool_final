#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<vector>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<thread>
#include<chrono>
#include<iostream>
#include<unordered_map>
#include<future>
#include<thread>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10; //��λ�� ��

enum class PoolMode {
	MODE_FIXED,   //�̶������߳�
	MODE_CACHED,   //�ɶ�̬���ӵ��߳�
};


//�߳�����
class Thread {
public:
	//�̺߳�����������
	using ThreadFunc = std::function<void(int)>;
	Thread(ThreadFunc func) : func_(func), 
		ThreadId_(generateId_++) {}
	~Thread() = default;
	//�����߳�
	void start() {
		//����һ���߳���ִ��һ���̺߳���
		std::thread t(func_, ThreadId_);
		t.detach();
	}
	//��ȡ�߳�ID
	int getId() const {
		return ThreadId_;
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int ThreadId_;
};
int Thread::generateId_ = 0;
/*
example:
Threaadpool pool;
pool.start(4);
class MyTask : public Task{
	public:
		virtual void run() override{
			// �̴߳���...
		}
}
pool.submitTask(std::make_shared<MyTask>());
*/
//�̳߳�����
class Threadpool {
public:
	Threadpool() :
		initThreadSize_(0),
		threadSizeThreshHold_(THREAD_MAX_THRESHHOLD),
		taskSize_(0),
		idleThreadSize_(0),
		curThreadSize_(0),
		taskQueMaxThreadHold_(TASK_MAX_THRESHHOLD),
		poolMode_(PoolMode::MODE_FIXED),
		isPoolRunning_(false) {}

	~Threadpool() {
		isPoolRunning_ = false;
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
	}

	Threadpool(const Threadpool&) = delete;
	Threadpool& operator=(const Threadpool&) = delete;

	//�����̳߳ع���ģʽ
	void setMode(PoolMode mode) {
		if (isPoolRunning_) {
			return;
		}
		poolMode_ = mode;
	}

	//����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshold) {
		taskQueMaxThreadHold_ = threshold;
	}

	//�����̳߳�cachedģʽ���߳���ֵ
	void setThreadSizeThreshHold(int threshold) {
		if (isPoolRunning_) {
			return;
		}
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshold;
		}
	}

	//���̳߳��ύ����
	template<typename Func,typename... Args>
	auto submitTask(Func&& func,Args&&... args) -> std::future<decltype(func(args...))>{

		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func),
			std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		//��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		//�̵߳�ͨ�� �ȴ���������п���,����������ܳ���1s
		if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool {
			return taskQue_.size() < (size_t)taskQueMaxThreadHold_;
			})) {
			std::cerr << "task queue is full,submit task fail," << std::endl;
			
			auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType{
				return RType();
				});
			(*task)();
			return task->get_future();
		}
		//����п��� ������������������
		//taskQue_.emplace(sp);
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;
		//��ʱ������в�Ϊ�գ���notEmpty_��֪ͨ
		notEmpty_.notify_all();

		//cachedģʽ��������ȽϽ���������:С���������
		//��Ҫ�������������Ϳ����߳��������ж��Ƿ���Ҫ�����µ��̳߳���
		if (poolMode_ == PoolMode::MODE_CACHED &&
			taskSize_ > idleThreadSize_ &&
			curThreadSize_ < threadSizeThreshHold_) {
			//�����µ��̶߳���
			std::cout << " >>> create new thread..." << std::endl;
			auto ptr = std::make_unique<Thread>(std::bind(&Threadpool::threadFunc, this, std::placeholders::_1));
			int threadID = ptr->getId();
			threads_.emplace(threadID, std::move(ptr));
			threads_[threadID]->start();
			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
	}

	//�����̳߳�
	void start(int initThreadSize = 4) {
		//�����̵߳�����״̬
		isPoolRunning_ = true;

		//��¼��ʼ�̸߳���
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;
		//�����̶߳���
		for (int i = 0; i < initThreadSize_; ++i) {
			auto ptr = std::make_unique<Thread>(std::bind(&Threadpool::threadFunc, this, std::placeholders::_1));
			int threadID = ptr->getId();
			threads_.emplace(threadID, std::move(ptr));
			//threads_.emplace_back(std::move(ptr));
		}
		//���������߳�
		for (int i = 0; i < initThreadSize_; ++i) {
			threads_[i]->start();
			idleThreadSize_++;  //��¼�����̵߳�����
		}
	}
private:
	//�����̺߳���
	void threadFunc(int threadId) {
		auto lastTime = std::chrono::high_resolution_clock().now();

		//�����������ִ����ɣ��̳߳زſ��Ի��������߳���Դ
		for (;;) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid : " << std::this_thread::get_id() << "���Ի�ȡ����..."
					<< std::endl;

				//cachedģʽ�£��п����Ѿ�����������̣߳����ǿ���ʱ�䳬��60s,Ӧ�ðѶ�����߳�
				//�������յ�(����initThreadSize_�������߳�Ҫ���л���)
				//��ǰʱ�� - ��һ���߳�ִ�е�ʱ�� > 60s 
				//�� + ˫���ж�
				while (taskQue_.size() == 0) {
					//�̳߳ؽ����������߳���Դ
					if (!isPoolRunning_) {
						threads_.erase(threadId);
						std::cout << "thread id :" << std::this_thread::get_id() << "exit!"
							<< std::endl;
						exitCond_.notify_all();
						return;   //�̺߳����������߳̽���
					}

					if (poolMode_ == PoolMode::MODE_CACHED) {
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {  //cv_statusΪwait_for����ֵ��ö���ࣩ
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME &&
								curThreadSize_ > initThreadSize_) {
								//���ո��߳�
								threads_.erase(threadId);
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "thread id :" << std::this_thread::get_id() << "exit!" << std::endl;
								return;
							}
						}
					}
					else {
						notEmpty_.wait(lock);
					}

					//if (!isPoolRunning_) {
					//	threads_.erase(threadId);
					//	std::cout << "thread id :" << std::this_thread::get_id()
					//		<< "exit!" << std::endl;
					//	exitCond_.notify_all();
					//	return;
					//}
				}

				idleThreadSize_--;

				std::cout << "tid : " << std::this_thread::get_id() << "��ȡ����ɹ�..."
					<< std::endl;

				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				//�����Ȼ������������֪ͨ�����߳�ִ������
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				notFull_.notify_all();

			}
			//��ǰ�߳�ִ�л�ȡ������
			if (task != nullptr) {
				task();
			}

			idleThreadSize_++; //����ִ����ɣ������߳�����++;
			lastTime = std::chrono::high_resolution_clock().now();  //�����߳�ִ���������ʱ��
		}
	}

	//���pool������״̬
	bool checkRunningState() const {
		return isPoolRunning_;
	}

	//std::vector<std::unique_ptr<Thread>> threads_; //�߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;
	int initThreadSize_;  //��ʼ���߳�����
	int threadSizeThreshHold_;//�߳�������������ֵ
	std::atomic_int curThreadSize_;//��¼��ǰ�̳߳������̵߳�����
	std::atomic_int idleThreadSize_;//��¼�����̵߳�����

	using Task = std::function<void()>;
	std::queue<Task> taskQue_;  //�������
	std::atomic_int taskSize_;//���������
	int taskQueMaxThreadHold_;  //�����������������ֵ

	std::mutex taskQueMtx_;  //��֤������е��̰߳�ȫ
	std::condition_variable notFull_;//��ʾ������в���
	std::condition_variable notEmpty_; //��ʾ������в���
	std::condition_variable exitCond_; // �ȴ��߳���Դȫ������

	//��ǰ�̳߳صĹ���ģʽ
	PoolMode poolMode_;

	std::atomic_bool isPoolRunning_;//��ʾ��ǰ�̵߳�����״̬
};

#endif