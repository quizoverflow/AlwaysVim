"""
2025.07 
개발자 : quizoverflow | GitHub 
Beta 0.1.0

1.command 기능이 아직 구현되지 않았음
2.맨 아랫줄에서 이동키 아래, 맨 윗줄에서 이동키 위 누르면 커서가 각각 맨 오른쪽, 왼쪽으로 가는 문제
3.  기타

"""

from command_window import *
from tool import *
import threading
from keyboard_controller import *
from data_controller import *

@singleton
class AwalysVim():

    def __init__(self):

        self.data = DataController()
        self.window = CommandWindow(self.data)

        custom = self.data.get_data()
        self.listener = KeyboardController(custom,self.window)


        # self.keyboard.remap_key()

        self.window.root.mainloop()


def main():
    always_vim = AwalysVim()

if __name__ == "__main__":
    main()




    