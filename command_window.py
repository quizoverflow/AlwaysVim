import tkinter as tk
from tool import *

@singleton
class CommandWindow():

    def __init__(self,data):
        self.data = data
        self.opacity = 1.0

        self.modeName = ["Normal","Insert","Visual"]
        self.modeColor = ["blue","white","yellow"]
        self.commandColor = "white"
        self.current_mode = 0
        self.root = tk.Tk()

        #표시줄 제거
        self.root.overrideredirect(True)


        self.root.geometry(self.data.location)
        self.root.attributes("-topmost",'true')
        self.root.attributes("-alpha",self.opacity)

        self.modeFrame = tk.Frame(self.root,width=50,height=20)
        self.commandFrame = tk.Frame(self.root,width=200,height=20)
        self.btnFrame = tk.Frame(self.root)
        self.moveFrame = tk.Frame(self.root,bg = "lightblue",width=20,height=20)

        self.modeLabel = tk.Label(self.modeFrame,text="Normal",bg = self.modeColor[0])
        self.commandLabel = tk.Label(self.commandFrame, bg = self.commandColor)
        self.btn = tk.Button(self.btnFrame,text="종료",command=self.exit_program)


        self.btnFrame.pack(side="left")
        self.modeFrame.pack(side= "left")
        self.commandFrame.pack(side = "left",padx=1)
        self.moveFrame.pack(side ="right")
        

        self.btn.pack()
        self.modeLabel.pack()
        self.commandLabel.pack()

        self.moveFrame.bind("<Button-1>",self.__on_button_press)
        self.moveFrame.bind("<B1-Motion>",self.__on_mouse_drag)
    
    def __on_button_press(self,event):
        self.x = event.x_root - self.root.winfo_x()
        self.y = event.y_root - self.root.winfo_y()
    
    def __on_mouse_drag(self,event):
        nx = event.x_root - self.x
        ny = event.y_root - self.y
        self.root.geometry(f"+{nx}+{ny}")
        

    def update_label(self,text_data):
        self.commandLabel.config(text=text_data)


    def change_mode(self,modeIdx):
        self.modeLabel.config(text=self.modeName[modeIdx],bg = self.modeColor[modeIdx])
        self.current_mode = modeIdx
    

    def exit_program(self):

        #종료전에 마지막 위치 저장
        loc = self.root.geometry()
        self.data.set_loc_data(loc)

        self.root.destroy()