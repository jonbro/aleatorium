unction resetParams()
    for i=0,7 do
        setParam(i, param.shape, shapes.csaw)
        setParam(i, param.timbre, 200)
        setParam(i, param.color, 100)
        setParam(i, param.env2Target, envTarget.cutoff*(255/6))
        setParam(i, param.env2Depth, 120)
        setParam(i, param.attackTime2, 0)
        setParam(i, param.decayTime2, 20)
        setParam(i, param.resonance, 80)
        setParam(i, param.cutoff, 160)
        setParam(i, param.attackTime, 0)
        setParam(i, param.decayTime, 50)
        setParam(i, param.delaySend, 0)
        
        setParam(i, param.volume, 205)
    end
end
resetParams()

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
function buildAccentTable()
    local accenttable = {}
    for i=1,32 do
        local a = {}
        if math.random() > 0.7 then
            table.insert(a, {param.decayTime, 100})
            table.insert(a, {param.env2Depth, 140})
        end
        table.insert(accenttable, a)
    end
    return accenttable
end
accenttable = buildAccentTable()

table.insert(b, getBleeper(96/4, function(s)
    local c = s.count%32+1
    resetParams()
    for k,v in ipairs(accenttable[c]) do
        setParam(0, v[1], v[2])
    end
    playNote(0, 70)
end))

table.insert(b, getBleeper(96/4, function(s)
    local c = (s.count+3)%32+1
    resetParams()
    for k,v in ipairs(accenttable[c]) do
        setParam(1, v[1], v[2])
    end
    playNote(1, 63)
end))

table.insert(b, getBleeper(96/2, function(s)
    local c = (s.count+3)%32+1
    resetParams()
    for k,v in ipairs(accenttable[c]) do
        setParam(2, v[1], v[2])
    end
    playNote(2, 65)
end))

table.insert(b, getBleeper(96*2, function(s)
    local c = (s.count+3)%32+1
    resetParams()
    for k,v in ipairs(accenttable[c]) do
        setParam(3, v[1], v[2])
    end
    if(math.random() > 0.7) then
        setParam(3, param.delaySend, 50)
    end
    playNote(3, 79)
end))

table.insert(b, getBleeper(96*2, function(s)
    if s.count%32 == 0 then
        accenttable = buildAccentTable()
    end
end))


function tempoSync()
    for k,v in ipairs(b) do
        v:trigger()
    end
    collectgarbage("collect")
end

