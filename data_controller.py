import yaml
from tool import *
import os

@singleton
class DataController():

    def __init__(self):
        self.current_dir = os.path.dirname(__file__)
        self.data_path = os.path.join(self.current_dir,'user','custom.yaml')
        
        self.data = self.get_data()

        self.key_normal = self.data["Normal"]
        self.key_insert = self.data["Insert"]
        
        self.location = self.data["Geometry"]
        

    def get_data(self) -> dict:
        with open(self.data_path,'r',encoding= 'utf-8') as f:
            res = yaml.safe_load(f)
        return res


    def set_loc_data(self,geo) -> None:
        self.data["Geometry"] = geo
        with open(self.data_path,'w',encoding= 'utf-8') as f:
            yaml.dump(self.data,f,allow_unicode=True)

        








    




    









