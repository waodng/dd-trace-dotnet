using System;
using System.Collections.Concurrent;
using System.Reflection;
using System.Reflection.Emit;
using Datadog.Trace.Util;

namespace Datadog.Trace.ClrProfiler.Emit
{
    /// <summary>
    /// Provides helper methods to access object members by emitting IL dynamically.
    /// </summary>
    internal static class ObjectExtensions
    {
        // A new module to be emitted in the current AppDomain which will contain DynamicMethods
        // and have same evidence/permissions as this AppDomain
        internal static readonly ModuleBuilder Module;

        private static readonly ConcurrentDictionary<MemberFetcherCacheKey, object> Cache = new ConcurrentDictionary<MemberFetcherCacheKey, object>();
        private static readonly ConcurrentDictionary<MemberFetcherCacheKey, IMemberFetcher> MemberFetcherCache = new ConcurrentDictionary<MemberFetcherCacheKey, IMemberFetcher>();

        static ObjectExtensions()
        {
#if NETSTANDARD
            var asm = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("Datadog.Trace.ClrProfiler.Emit.DynamicAssembly"), AssemblyBuilderAccess.Run);
#else
            var asm = AppDomain.CurrentDomain.DefineDynamicAssembly(new AssemblyName("Datadog.Trace.ClrProfiler.Emit.DynamicAssembly"), AssemblyBuilderAccess.Run);
#endif
            Module = asm.DefineDynamicModule("DynamicModule");
        }

        /// <summary>
        /// Tries to call an instance method with the specified name, a single parameter, and a return value.
        /// </summary>
        /// <typeparam name="TArg1">The type of the method's single parameter.</typeparam>
        /// <typeparam name="TResult">The type of the method's result value.</typeparam>
        /// <param name="source">The object to call the method on.</param>
        /// <param name="methodName">The name of the method to call.</param>
        /// <param name="arg1">The value to pass as the method's single argument.</param>
        /// <param name="value">The value returned by the method.</param>
        /// <returns><c>true</c> if the method was found, <c>false</c> otherwise.</returns>
        public static bool TryCallMethod<TArg1, TResult>(this object source, string methodName, TArg1 arg1, out TResult value)
        {
            var type = source.GetType();
            var paramType1 = typeof(TArg1);

            object cachedItem = Cache.GetOrAdd(
                new MemberFetcherCacheKey(MemberType.Method, type, paramType1, methodName),
                key =>
                    DynamicMethodBuilder<Func<object, TArg1, TResult>>
                       .CreateMethodCallDelegate(
                            key.Type1,
                            key.Name,
                            OpCodeValue.Callvirt,
                            methodParameterTypes: new[] { key.Type2 }));

            if (cachedItem is Func<object, TArg1, TResult> func)
            {
                value = func(source, arg1);
                return true;
            }

            value = default;
            return false;
        }

        /// <summary>
        /// Tries to call an instance method with the specified name, two parameters, and no return value.
        /// </summary>
        /// <typeparam name="TArg1">The type of the method's first parameter.</typeparam>
        /// <typeparam name="TArg2">The type of the method's second parameter.</typeparam>
        /// <param name="source">The object to call the method on.</param>
        /// <param name="methodName">The name of the method to call.</param>
        /// <param name="arg1">The value to pass as the method's first argument.</param>
        /// <param name="arg2">The value to pass as the method's second argument.</param>
        /// <returns><c>true</c> if the method was found, <c>false</c> otherwise.</returns>
        public static bool TryCallVoidMethod<TArg1, TArg2>(this object source, string methodName, TArg1 arg1, TArg2 arg2)
        {
            var type = source.GetType();
            var paramType1 = typeof(TArg1);
            var paramType2 = typeof(TArg2);

            object cachedItem = Cache.GetOrAdd(
                new MemberFetcherCacheKey(MemberType.Method, type, paramType1, paramType2, methodName),
                key =>
                    DynamicMethodBuilder<Action<object, TArg1, TArg2>>
                       .CreateMethodCallDelegate(
                            key.Type1,
                            key.Name,
                            OpCodeValue.Callvirt,
                            methodParameterTypes: new[] { key.Type2, key.Type3 }));

            if (cachedItem is Action<object, TArg1, TArg2> func)
            {
                func(source, arg1, arg2);
                return true;
            }

            return false;
        }

        /// <summary>
        /// Tries to call an instance method with the specified name and a return value.
        /// </summary>
        /// <typeparam name="TResult">The type of the method's result value.</typeparam>
        /// <param name="source">The object to call the method on.</param>
        /// <param name="methodName">The name of the method to call.</param>
        /// <param name="value">The value returned by the method.</param>
        /// <returns><c>true</c> if the method was found, <c>false</c> otherwise.</returns>
        public static bool TryCallMethod<TResult>(this object source, string methodName, out TResult value)
        {
            var type = source.GetType();

            object cachedItem = Cache.GetOrAdd(
                new MemberFetcherCacheKey(MemberType.Method, type, null, methodName),
                key =>
                    DynamicMethodBuilder<Func<object, TResult>>
                       .CreateMethodCallDelegate(
                            key.Type1,
                            key.Name,
                            OpCodeValue.Callvirt));

            if (cachedItem is Func<object, TResult> func)
            {
                value = func(source);
                return true;
            }

            value = default;
            return false;
        }

        public static MemberResult<TResult> CallMethod<TArg1, TResult>(this object source, string methodName, TArg1 arg1)
        {
            return source.TryCallMethod(methodName, arg1, out TResult result)
                       ? new MemberResult<TResult>(result)
                       : MemberResult<TResult>.NotFound;
        }

        public static MemberResult<object> CallMethod<TArg1>(this object source, string methodName, TArg1 arg1)
        {
            return CallMethod<TArg1, object>(source, methodName, arg1);
        }

        public static MemberResult<TResult> CallMethod<TResult>(this object source, string methodName)
        {
            return source.TryCallMethod(methodName, out TResult result)
                       ? new MemberResult<TResult>(result)
                       : MemberResult<TResult>.NotFound;
        }

        public static MemberResult<object> CallVoidMethod<TArg1, TArg2>(this object source, string methodName, TArg1 arg1, TArg2 arg2)
        {
            return source.TryCallVoidMethod(methodName, arg1, arg2)
                       ? new MemberResult<object>(null)
                       : MemberResult<object>.NotFound;
        }

        /// <summary>
        /// Tries to get the value of an instance property with the specified name.
        /// </summary>
        /// <typeparam name="TResult">The type of the property.</typeparam>
        /// <param name="source">The value that contains the property.</param>
        /// <param name="propertyName">The name of the property.</param>
        /// <param name="value">The value of the property, or <c>null</c> if the property is not found.</param>
        /// <returns><c>true</c> if the property exists, otherwise <c>false</c>.</returns>
        public static bool TryGetPropertyValue<TResult>(this object source, string propertyName, out TResult value)
        {
            if (source != null)
            {
                var type = source.GetType();

                IMemberFetcher fetcher = MemberFetcherCache.GetOrAdd(
                    GetKey<TResult>(MemberType.Property, propertyName, type),
                    key => new PropertyFetcher(key.Name));

                if (fetcher != null)
                {
                    value = fetcher.Fetch<TResult>(source, type);
                    return true;
                }
            }

            value = default;
            return false;
        }

        public static MemberResult<TResult> GetProperty<TResult>(this object source, string propertyName)
        {
            if (source == null)
            {
                return MemberResult<TResult>.NotFound;
            }

            return source.TryGetPropertyValue(propertyName, out TResult result)
                       ? new MemberResult<TResult>(result)
                       : MemberResult<TResult>.NotFound;
        }

        public static MemberResult<object> GetProperty(this object source, string propertyName)
        {
            return GetProperty<object>(source, propertyName);
        }

        /// <summary>
        /// Tries to get the value of an instance field with the specified name.
        /// </summary>
        /// <typeparam name="TResult">The type of the field.</typeparam>
        /// <param name="source">The value that contains the field.</param>
        /// <param name="fieldName">The name of the field.</param>
        /// <param name="value">The value of the field, or <c>null</c> if the field is not found.</param>
        /// <returns><c>true</c> if the field exists, otherwise <c>false</c>.</returns>
        public static bool TryGetFieldValue<TResult>(this object source, string fieldName, out TResult value)
        {
            if (source != null)
            {
                var type = source.GetType();

                IMemberFetcher fetcher = MemberFetcherCache.GetOrAdd(
                    GetKey<TResult>(MemberType.Field, fieldName, type),
                    key => new FieldFetcher(key.Name));

                if (fetcher != null)
                {
                    value = fetcher.Fetch<TResult>(source, type);
                    return true;
                }
            }

            value = default;
            return false;
        }

        public static MemberResult<TResult> GetField<TResult>(this object source, string fieldName)
        {
            return source.TryGetFieldValue(fieldName, out TResult result)
                       ? new MemberResult<TResult>(result)
                       : MemberResult<TResult>.NotFound;
        }

        public static MemberResult<object> GetField(this object source, string fieldName)
        {
            return GetField<object>(source, fieldName);
        }

        private static MemberFetcherCacheKey GetKey<TResult>(MemberType memberType, string name, Type type)
        {
            return new MemberFetcherCacheKey(memberType, type, typeof(TResult), name);
        }

        private readonly struct MemberFetcherCacheKey : IEquatable<MemberFetcherCacheKey>
        {
            public readonly MemberType MemberType;
            public readonly Type Type1;
            public readonly Type Type2;
            public readonly Type Type3;
            public readonly string Name;

            public MemberFetcherCacheKey(MemberType memberType, Type type1, Type type2, string name)
                : this(memberType, type1, type2, null, name)
            {
            }

            public MemberFetcherCacheKey(MemberType memberType, Type type1, Type type2, Type type3, string name)
            {
                MemberType = memberType;
                Type1 = type1 ?? throw new ArgumentNullException(nameof(type1));
                Type2 = type2;
                Type3 = type3;
                Name = name ?? throw new ArgumentNullException(nameof(name));
            }

            public bool Equals(MemberFetcherCacheKey other)
            {
                return Equals(Type1, other.Type1) && Equals(Type2, other.Type2) && Equals(Type3, other.Type3) && Name == other.Name;
            }

            public override bool Equals(object obj)
            {
                return obj is MemberFetcherCacheKey other && Equals(other);
            }

            public override int GetHashCode()
            {
                unchecked
                {
                    var hashCode = Type1.GetHashCode();
                    hashCode = (hashCode * 397) ^ (Type2 != null ? Type2.GetHashCode() : 0);
                    hashCode = (hashCode * 397) ^ (Type3 != null ? Type3.GetHashCode() : 0);
                    hashCode = (hashCode * 397) ^ Name.GetHashCode();
                    return hashCode;
                }
            }
        }
    }
}
