[Statedef 0000]
type=S
physics=S
sprpriority=0
ctrl=0
[State *]
type=changeanim
trigger1=(anim!=0&&anim!=5)||(anim=5&&!animtime)
value=0
[State *]
type=velset
trigger1=!time
y=0
[State *]
type=velset
trigger1=abs(vel x)<2
trigger2=time=4
x=0
[State *]
type=changestate
trigger1=fvar(09)>=1
trigger1=life<=0
value=5150
[State *]
type=changestate
triggerall=fvar(09)>=1
trigger1=numpartner
trigger1=id<partner,id
trigger2=!numpartner
value=1
[State *]
type=changestate
triggerall=fvar(09)>=1
trigger1=numpartner
trigger1=id>partner,id
value=3
[Statedef 0001]
type=A
physics=N
sprpriority=0
ctrl=0
[State *]
type=screenbound
trigger1=roundstate>=2
value=0
movecamera=0,0
[State *]
type=null;changestate
trigger1=enemy(0),stateno=49901&&enemy(0),time=16&&enemy(0),fvar(33)=0
value=0
[State *]
type=changestate
trigger1=numpartner
trigger1=id>partner,id
trigger1=enemy,sysvar(2)=0
trigger1=enemy,fvar(09)>=1
value=2
[State *]
type=null
triggerall=enemy,fvar(38)>0
triggerall=enemy,stateno!=[5900,5901]
triggerall=enemy,stateno!=[40000,49900]
trigger1=var(43)=0
trigger1=0&&var(43):=ifelse(enemy,fvar(14)<1||selfanimexist(10000000+((enemy,fvar(14)=1)*(enemy,var(42))*1000))=1,1,(selfanimexist(10000000+((enemy,fvar(14)=1)*(enemy,var(42))*1000))=0)*2)
trigger2=var(44)=0
trigger2=0&&var(44):=ifelse(enemy,fvar(14)<1||selfanimexist(11000000+((enemy,fvar(14)=1)*(enemy,var(42))*1000))=1,1,(selfanimexist(11000000+((enemy,fvar(14)=1)*(enemy,var(42))*1000))=0)*2)
trigger3=var(45)=0
trigger3=0&&var(45):=ifelse(enemy,fvar(14)<1||selfanimexist(12000000+((enemy,fvar(14)=1)*(enemy,var(42))*1000))=1,1,(selfanimexist(12000000+((enemy,fvar(14)=1)*(enemy,var(42))*1000))=0)*2)
[State *]
type=helper
trigger1=!numhelper(790)
trigger1=abs(var(59))=80
helpertype=normal
name="door"
id=790
pos=562,0
postype=left
stateno=5
facing=1
keyctrl=0
ownpal=1
ignorehitpause=1
[State *]
type=helper
trigger1=!numhelper(800)
trigger1=abs(var(59))=80
helpertype=normal
name="heal_shit"
id=800
pos=128,0
postype=left
stateno=13
facing=1
keyctrl=0
ownpal=1
ignorehitpause=1
[State *];renon
type=helper
trigger1=!numhelper(50000)
trigger1=abs(var(59))=80
trigger1=var(43)=1
helpertype=normal
name="renon"
id=50000
pos=224,0
postype=left
stateno=50000
facing=-1
keyctrl=0
ownpal=1
ignorehitpause=1
[State *]
type=helper
trigger1=!numhelper(50010)
trigger1=abs(var(59))=80
trigger1=var(44)=1
helpertype=normal
name="germain"
id=50010
pos=296,0
postype=left
stateno=50010
facing=-1
keyctrl=0
ownpal=1
ignorehitpause=1
[State *]
type=null;helper
trigger1=!numhelper(50020)
trigger1=abs(var(59))=80
trigger1=var(45)=1
helpertype=normal
name="aeon"
id=50020
pos=360,0
postype=left
stateno=50020
facing=-1
keyctrl=0
ownpal=1
ignorehitpause=1
[State *]
type=varset
trigger1=var(01)>=1
trigger1=numhelper(800)
trigger1=abs(helper(800),p2dist x)>12
var(01)=0
ignorehitpause=1
[State *]
type=varset
trigger1=var(01)=0
trigger1=numhelper(800)
trigger1=abs(helper(800),p2dist x)<=12
trigger2=var(01)=2
trigger2=helper(800),var(01)=0
var(01)=1
ignorehitpause=1
[State *]
type=varset
trigger1=var(01)=1
trigger1=numhelper(800)
trigger1=abs(helper(800),p2dist x)<=12
trigger1=helper(800),var(01)>=1
var(01)=2
ignorehitpause=1
[State *]
type=ctrlset
trigger1=ctrl=1
value=0
[State *]
type=changestate
trigger1=numhelper(790)
trigger1=helper(790),anim=791;&&helper(790),animelem=21,>=0
value=2
[Statedef 0002]
type=A
physics=N
sprpriority=0
ctrl=0
[State *]
type=lifeset
trigger1=numhelper(790)
trigger1=life>0&&helper(790),anim=791&&helper(790),animelem=21,>=0
value=-(lifemax*9999)
[State *]
type=changestate
trigger1=numhelper(790)
trigger1=life<=0&&helper(790),anim=791&&helper(790),animelem=21,>=0
value=5150
[Statedef 0003]
type=A
physics=N
sprpriority=0
ctrl=0
[State *]
type=lifeset
trigger1=enemy,sysvar(2)=0
trigger1=enemy,fvar(09)>=1
trigger1=1
value=-(lifemax*9999)
[State *]
type=changestate
trigger1=enemy,sysvar(2)=0
trigger1=enemy,fvar(09)>=1
trigger1=life<=0
value=5150
[Statedef 0005]
type=A
physics=N
sprpriority=-20
ctrl=0
anim=790
[State *]
type=removeexplod
trigger1=(root,numhelper(63000)||root,numhelper(64100))
trigger2=numenemy
trigger2=(enemy,numhelper(63000)||enemy,numhelper(64100))
trigger3=numpartner
trigger3=(partner,numhelper(63000)||partner,numhelper(64100))
ignorehitpause=1
[State *]
type=destroyself
trigger1=(root,numhelper(63000)||root,numhelper(64100))
trigger2=numenemy
trigger2=(enemy,numhelper(63000)||enemy,numhelper(64100))
trigger3=numpartner
trigger3=(partner,numhelper(63000)||partner,numhelper(64100))
ignorehitpause=1
[State *]
type=displaytoclipboard
trigger1=1
text="%d,%d,%d,%d,%d"
params=anim=790,(p2dist y=[-5,5]),enemy,command="holdfwd",((enemy,anim=20)||(enemy,stateno=20)||(enemy,authorname="CvF"&&enemy,stateno=6)||(enemy,authorname="enemy"&&enemy,stateno=6)),abs(root,p2dist x)>=430;853
ignorehitpause=1
[State *]
type=appendtoclipboard
trigger1=1
text=",%f,explod=%d"
params=abs(root,p2dist x),numexplod
[State *]
type=playsnd
triggerall=anim=790&&(p2dist y=[-5,5])&&(enemy,command="holdfwd"||enemy,stateno=188)
triggerall=((enemy,anim=20)||(enemy,anim=100)||(enemy,stateno=100)||(enemy,stateno=20)||(enemy,authorname="CvF"&&enemy,stateno=6)||(enemy,authorname="enemy"&&enemy,stateno=6))
trigger1=enemy,name!="Slogra"&&abs(root,p2dist x)>=430
trigger2=enemy,name="Slogra"&&abs(root,p2dist x)>=430
value=F0,179
pan=-floor((160-screenpos x)*3)*facing
volume=floor(256-(abs((screenpos x-160)*0.40)+abs((screenpos y-120)*0.40)))
volumescale=100-(abs((screenpos x-160)*0.185)+abs((screenpos y-120)*0.185))
ignorehitpause=1
[State *]
type=changeanim
triggerall=anim=790&&abs(p2dist y)<=5&&(enemy,command="holdfwd"||enemy,stateno=188)
triggerall=((enemy,anim=20)||(enemy,stateno=20)||(enemy,anim=100)||(enemy,stateno=100)||(enemy,authorname="CvF"&&enemy,stateno=6)||(enemy,authorname="enemy"&&enemy,stateno=6))
trigger1=enemy,name!="Slogra"&&abs(root,p2dist x)>=430
trigger2=enemy,name="Slogra"&&abs(root,p2dist x)>=430
value=791
ignorehitpause=1
[Statedef 0013];heal shit
type=S
anim=800
sprpriority=-80
[State *]
type=removeexplod
trigger1=(root,numhelper(63000)||root,numhelper(64100))
trigger2=numenemy
trigger2=(enemy,numhelper(63000)||enemy,numhelper(64100))
trigger3=numpartner
trigger3=(partner,numhelper(63000)||partner,numhelper(64100))
ignorehitpause=1
[State *]
type=destroyself
trigger1=(root,numhelper(63000)||root,numhelper(64100))
trigger2=numenemy
trigger2=(enemy,numhelper(63000)||enemy,numhelper(64100))
trigger3=numpartner
trigger3=(partner,numhelper(63000)||partner,numhelper(64100))
ignorehitpause=1
[State *]
type=null
trigger1=!time||(enemy,stateno=190)
trigger1=0&&var(58):=8&&var(59):=10
[State *]
type=helper
trigger1=!numhelper(65532)
helpertype=normal
name="dir_down"
id=65532
pos=0,-64
postype=p1
stateno=65532
facing=1
keyctrl=0
ownpal=1
ignorehitpause=1
[State *]
type=explod
trigger1=time%7=0
trigger1=screenpos x=[-50,370]
anim=F249
id=244
pos=0,-30
postype=p1
facing=ifelse(random>500,1,-1)
bindtime=1
removetime=-2
sprpriority=-81
random=2,4
vel=0,-0.5
scale=0.7,0.7
ownpal=1
ignorehitpause=1
[State *]
type=varset
triggerall=var(01)=1
trigger1=enemynear(0),anim%10000!=[10,11]
trigger2=abs(p2dist x)>var(58)
trigger3=abs(p2dist y)>var(59)
var(01)=0
ignorehitpause=1
[State *]
type=varset
triggerall=var(01)=0
triggerall=abs(p2dist x)<=var(58)
triggerall=abs(p2dist y)<=var(59)
trigger1=(enemynear(0),stateno=[10,11])&&(enemynear(0),anim%10000=6||enemynear(0),anim%10000=10||enemynear(0),anim%10000=11)
trigger1=enemynear(0),command="holddown"
trigger2=enemynear(0),stateno=38
trigger2=enemynear(0),command="holddown"
var(01)=1
ignorehitpause=1
[State *]
type=explod
trigger1=var(01)=1&&numexplod(801)=0
anim=801
id=801
pos=0,0
postype=p1
facing=1
bindtime=1
removetime=-1
sprpriority=-82
ownpal=1
trans=addalpha
alpha=256,256
ignorehitpause=1
[State *]
type=null
trigger1=var(01)=1&&var(02)=0
trigger1=0&&var(02):=1&&var(03):=0
trigger2=var(02)=1&&var(03)<32
trigger2=0&&var(03):=var(03)+1
trigger3=var(02)=1&&var(03)>=32
trigger3=0&&var(02):=2
trigger4=var(02)=2&&var(03)>0
trigger4=0&&var(03):=var(03)-1
trigger5=var(02)=2&&var(03)<=0
trigger5=0&&var(02):=1
trigger6=var(01)!=1&&var(02)>0
trigger6=0&&var(02):=0&&var(03):=0
ignorehitpause=1
[State *]
type=modifyexplod
trigger1=numexplod(801)
id=801
pos=0,0
postype=p1
trans=addalpha
alpha=var(03)*8,256
ignorehitpause=1
[State *]
type=removeexplod
trigger1=numexplod(801)
trigger1=var(01)!=1
id=801
ignorehitpause=1
[State *]
type=explod
trigger1=var(01)=1
anim=F70
id=70
pos=0,-16
postype=p1
facing=1
bindtime=1
removetime=-2
sprpriority=-4
random=16,16
vel=0,-2
ownpal=1
ignorehitpause=1
[State *]
type=displaytoclipboard
trigger1=1
text="%d,%d,%d,%d,explod=%d"
params=var(01),var(02),var(03),var(59),numexplod
ignorehitpause=1
[Statedef 0190]
type=S
ctrl=0
anim=1
velset=0,0
[State *]
type=varset
triggerall=time>=0
triggerall=enemy,stateno!=892&&enemy,stateno!=49900
triggerall=enemy,fvar(38)>0
trigger1=enemy,stateno=[49000,49999]
trigger2=enemy,stateno=[49000,49999]
fvar(09)=1
ignorehitpause=1
[State *]
type=changestate
trigger1=fvar(09)>=1
value=1
ctrl=0
[Statedef 4997]
type=A
movetype=H
physics=N
anim=1
ctrl=0
sprpriority=0
[State *]
type=bindtoparent
trigger1=1
time=1
pos=0,0
ignorehitpause=1
[State *]
type=palfx
trigger1=1
time=1
add=0,0,0
color=256
ignorehitpause=1
[State *]
type=explod
trigger1=time<180&&gametime%2=0
anim=F50
id=6000
pos=-6,-30
random=12,12
postype=p1
facing=1
bindtime=1
vel=0.6,-0.4
removetime=30
sprpriority=9
ownpal=1
ignorehitpause=1
[State *]
type=explod
trigger1=time<190&&gametime%2=1
anim=F50
id=600
pos=4,-26
random=12,12
postype=p1
facing=1
bindtime=1
vel=0.6,-0.4
removetime=30
sprpriority=9
ownpal=1
ignorehitpause=1
[State *]
type=explod
trigger1=time<185&&gametime%2=0
anim=F50
id=600
pos=14,-20
random=12,12
postype=p1
facing=1
bindtime=1
vel=0.6,-0.4
removetime=30
sprpriority=9
ownpal=1
ignorehitpause=1
[State *]
type=destroyself
trigger1=time>=192
[Statedef 4998]
type=A
movetype=H
physics=N
anim=1
ctrl=0
velset=0,0
sprpriority=0
[State *]
type=changestate
trigger1=1
value=5150
ctrl=0
ignorehitpause=1
[Statedef 5150]
type=L
movetype=H
physics=N
sprpriority=-3
ctrl=0
velset=0,0
[State *]
type=varset
trigger1=sysvar(2)!=0
sysvar(2)=0
ignorehitpause=1
[State *]
type=varset
triggerall=enemy,life>0||enemy,var(52)>0
trigger1=!numpartner
trigger2=numpartner
trigger2=partner,stateno=5150
var(35)=0
[State *]
type=varset
triggerall=enemy,life>0||enemy,var(52)>0
trigger1=!numpartner
trigger2=numpartner
trigger2=partner,stateno=5150
var(36)=0
[State *]
type=varset
triggerall=enemy,life>0||enemy,var(52)>0
trigger1=!numpartner
trigger2=numpartner
trigger2=partner,stateno=5150
var(37)=0
[State *]
type=varset
triggerall=enemy,life>0||enemy,var(52)>0
trigger1=!numpartner
trigger2=numpartner
trigger2=partner,stateno=5150
var(38)=0
[State *]
type=lifeset
trigger1=1
value=-(lifemax*9999)
[State *]
type=trans
trigger1=numpartner
trigger1=var(58)>=1
trigger1=selfanimexist(9000550+var(51))=1
trigger1=time=[64,96]
trans=addalpha
alpha=256-((time-64)*8),0+((time-64)*8)
ignorehitpause=1
[State *]
type=assertspecial
trigger1=numpartner
trigger1=var(58)>=1
trigger1=selfanimexist(9000550+var(51))=1
trigger1=time>=96
flag=Invisible
ignorehitpause=1
[State *]
type=posset
trigger1=numpartner&&numenemy
trigger1=!(fvar(39)=0&&enemy,fvar(39)=0&&partner,name="intro")
trigger1=fvar(09)>0&&enemy(0),fvar(09)>0
trigger1=numhelper(10000)||enemy,numhelper(10000)
trigger1=numhelper(54000)&&numhelper(54001)
trigger1=enemy,numhelper(54000)&&enemy,numhelper(54001)
trigger1=partner,numhelper(54000)&&partner,numhelper(54001)
trigger1=partner,numhelper(54000)&&partner,numhelper(54001)
x=9999
y=1000
ignorehitpause=1
