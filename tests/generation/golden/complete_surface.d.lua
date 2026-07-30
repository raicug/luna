





declare class Studio_Shape
  read Corners: number
end


declare class Studio_Gadget extends Studio_Shape

  function Boost(self: Studio_Gadget, Argument1: number): number
  Charge: number

  read Level: number
  function __len(self: Studio_Gadget): number
end

declare function Describe(Argument1: string): string


declare Physics: {
  Blend: ((number, number) -> number) & ((string) -> string),
  Enabled: boolean,
  Epsilon: number,

  Gravity: number,
  Offset: (number, number?) -> number,
  Reset: () -> (),

  Scale: (number) -> number,

  Solver: {
    Iterations: number,
    Solve: (number) -> number,
  },
  Split: (string) -> (number, string),
  Total: (...any) -> number,
  Trim: (number, number?) -> number,
}

declare Studio: {

  Gadget: {
    Describe: () -> string,
    FromCharge: (number) -> Studio_Gadget,
    New: () -> Studio_Gadget,
  },

  Palette: {
    Blue: number,
    Green: number,
    Primary: number,

    Red: number,
  },

  Shape: {
    New: () -> Studio_Shape,
  },
}


declare Units: {

  Metre: number,
  Name: string,
}

declare Version: string
