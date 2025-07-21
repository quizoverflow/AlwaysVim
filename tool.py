import os

# decorator
def singleton(cls):
    instances = {}
    def get_instance(*args, **kwargs):
        if cls not in instances:
            instances[cls] = cls(*args, **kwargs)
        return instances[cls]
    return get_instance

#decorator
def logging(func):
    def wrapper(*args, **kwargs):
        print(f"{func.__name__} called")
        print(f"{func.__name__} finished")
    return wrapper

#debuger
class Debuger():
    @staticmethod
    def printd(msg):
        print(f"[DBG] {msg}",flush=True)
    @staticmethod
    def printc(msg):
        os.system("cls")
        print(f"[DBG] {msg}",flush = True)