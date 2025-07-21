import keyboard
from tool import *
from data_controller import DataController
import threading
import time


@singleton
class KeyboardController():
    def __init__(self,custom,window) :
        self.key = None
        self.custom = custom
        self.window = window
        self.isRemapOn = True
        self.mode = 0
        self.label_text = ""
        self.command = []
        self.normal_keymap = self.custom["Normal"]

        #후킹된 키 관리. 언훅에 필요함
        self.hook_list = []

        #normal mode 진입 키 (default key = esc)
        self.normal_key = list(self.custom["Insert"].keys())[0]
        """
        mode
        normal = 0, insert = 1 visual = 2 , command 3
        (사용자는 normal, command가 하나의 모드로만 보이지만, 내부적으로는 normal과 commmand 모드는 구분되어 작동된다.)
        """
        self.mode_list = [self.normal_mode,self.insert_mode,self.visual_mode,self.command_mode]

        self.remap_key()
        key_listener_thread = threading.Thread(target= self.key_listener,daemon=True)
        key_listener_thread.start()


    #flow start
    def key_listener(self):
        keyboard.on_press(self.handle_key)
        keyboard.wait()

    #flow 2
    def handle_key(self,event):
        self.key = event.name
        print(f"{event.name} pressed  | current mode = {self.mode}")

        self.command.append(self.key)

        self.mode_list[self.mode]()
        

    def normal_mode(self):
        #시작시 한 번 실행을 위해
        if self.isRemapOn == False:
            self.remap_key()
            self.window.change_mode(0)

    def insert_mode(self):
        #normal mode로 진입 감시
        if self.key == self.normal_key:
            self.mode = 0
            self.window.change_mode(0)
            self.remap_key()

    def visual_mode(self):
        #normal mode로 진입 감시. (블럭해제)
        if self.key == self.normal_key:
            self.mode = 1
            self.window.change_mode(0)
        keyboard.release("shift")
        keyboard.release("left")

    def command_mode(self):
        pass
    

    def update_command(self):
        for command in self.command:
            self.label_text += f" {command}"
        self.window.update_label(self.label_text)

    def unhook_all(self):
        if len(self.hook_list) == 0:
            return
        for id in self.hook_list:
            keyboard.unhook(id)
        print("unhook complete")


    #normal mode와 visual mode에서의 키 바인딩
    #remap 함수에서 custom.yaml를 디코드하느라 볼륨이 커지고 있음. 분리시켜야 함.
    #decode.py 모듈에서 decoder 객체를 생성해야 할듯듯

    def remap_key(self):
        self.isRemapOn = True
        def remap(event):
            if event.event_type == 'down':
                for code in self.normal_keymap[event.name]:
                    #모드 변경
                    if code[0:4] == "mode":
                        #command mode
                        if code[-1] == "3":
                            self.mode = 3
                            self.isRemapOn = False
                            self.unhook_all()
                        #normal -> visual , visual -> normal (normal모드에서만 진입가능함)
                        elif code[-1] == "2":
                            if self.mode == 0:
                                self.mode = 2
                                self.window.change_mode(2)
                                keyboard.press("shift")
                            else:
                                self.mode = 0
                                self.window.change_mode(0)
                                keyboard.release("shift")
                                keyboard.send("left") # 블럭해제
                                
                        #insert mode
                        elif code[-1] == "1":
                            self.mode = 1
                            self.window.change_mode(1)
                            self.unhook_all()

                    #시간지연
                    elif code[0:5] == "sleep":
                        sleep_time = float(code[6:-1])
                        time.sleep(sleep_time)

                    #키차단
                    elif code[0:5] == "block":
                        keyboard.block_key(code[6:])
                    
                    #키차단해제
                    elif code[0:7] == "unblock":
                        keyboard.unblock_key(code[8:])

                    #키 떼기
                    elif  code[0:7] == "release":
                        keyboard.release(code[8:])

                    else:
                        keyboard.send(code)

        for key in self.normal_keymap:
            id = keyboard.on_press_key(key,remap,suppress=True)
            self.hook_list.append(id)
        print("[Key Remapping Started in Thread]")