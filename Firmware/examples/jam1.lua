resetParams()
for i=0,7 do
setParam(i, param.shape, shapes.struck_bell+1)
setParam(i, param.timbre, 200)
setParam(i, param.color, 100)
setParam(i, param.attackTime, 0)
setParam(i, param.decayTime, 255)
setParam(i, param.volume, 205)
end
function getBleeper(c, fn)
    local bl = {}
    bl.countdown = c
    bl.reset = c
    bl.fn = fn
    bl.count = 0
    function bl:trigger()
        self.countdown = self.countdown - 1
        if(self.countdown == 0) then
            self.countdown = self.reset
            self:fn()
            self.count = self.count + 1
        end
    end
    return bl
end
local b = {}

table.insert(b, getBleeper(96*8, function(s)
    playNote(0, 70)
end))
table.insert(b, getBleeper(96*12, function(s)
    playNote(1, 70-7)
end))
table.insert(b, getBleeper(96*20, function(s)
    playNote(2, 73)
end))


function tempoSync()
    for k,v in ipairs(b) do
        v:trigger()
    end
end
